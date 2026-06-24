#pragma once

#include "worker_thread.hpp"
#include "blocking_pool.hpp"
#include <elio/log/macros.hpp>
#include <elio/coro/frame.hpp>
#include <elio/coro/vthread_stack.hpp>
#include <elio/coro/task.hpp>
#include <elio/coro/traits.hpp>
#include <type_traits>
#include <array>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <coroutine>
#include <csignal>
#include <thread>
#include <algorithm>
#include <functional>
#include <vector>

namespace elio::runtime {

class scheduler;

namespace detail {
    using elio::detail::task_value_t;
    using elio::detail::is_task;
    using elio::detail::is_task_v;

    /// Wrapper coroutine for fire-and-forget (go/go_to).
    /// Stores callable and args in its coroutine frame, ensuring lambda lifetime
    /// safety even when temporary lambdas are passed.
    ///
    /// Tracked-task accounting is NOT done in a body-local guard here. Doing
    /// so would leave a window where the wrapper had been pushed to a worker
    /// inbox (post-spawn) but not yet resumed (pre-body-ctor) — during that
    /// window active_tracked_=0 and the inbox briefly drops to 0 between the
    /// worker's pop and its handle.resume(), which makes wait_for_idle
    /// race-prone (it can observe active_tasks()==0 and return drained=true
    /// while the task is genuinely still in flight). Instead, the +1 is
    /// done at the ``sched.go`` call site (before do_spawn) and the -1 is
    /// done in promise_base::~promise_base via the on_spawn_completion_
    /// callback, which fires whenever the frame is destroyed regardless of
    /// whether the body ever ran.
    template<typename F, typename... Args>
        requires (std::invocable<F, Args...> && is_task_v<std::invoke_result_t<F, Args...>>)
    coro::task<void> callable_wrapper_void(F f, Args... args) {
        co_await std::invoke(std::move(f), std::move(args)...);
    }

    /// Wrapper coroutine for joinable spawn (go_joinable).
    /// Same lifetime safety as callable_wrapper_void, but preserves return value.
    template<typename F, typename... Args>
        requires (std::invocable<F, Args...> && is_task_v<std::invoke_result_t<F, Args...>>)
    auto callable_wrapper(F f, Args... args) -> std::invoke_result_t<F, Args...> {
        co_return co_await std::invoke(std::move(f), std::move(args)...);
    }
} // namespace detail

/// Work-stealing scheduler for coroutines
class scheduler {
    friend class worker_thread;  // Allow workers to set current_scheduler_
    
public:
    static constexpr size_t MAX_THREADS = 256;

    explicit scheduler(size_t num_threads = std::thread::hardware_concurrency(),
                       wait_strategy strategy = wait_strategy::blocking(),
                       size_t blocking_threads = 4)
        : num_threads_(num_threads == 0 ? 1 : num_threads)
        , running_(false)
        , paused_(false)
        , spawn_index_(0)
        , wait_strategy_(strategy)
        , blocking_pool_(std::make_unique<blocking_pool>(blocking_threads)) {

        size_t n = num_threads_.load(std::memory_order_relaxed);
        // Fixed-size storage avoids the data race vector::push_back triggers
        // when set_thread_count() grows the pool concurrently with hot-path
        // readers (get_worker, schedule_round_robin, active_tasks, ...).
        // Slots are populated in-place; the num_threads_ atomic publishes the
        // visible range to readers via release/acquire.
        for (size_t i = 0; i < n; ++i) {
            workers_[i] = std::make_unique<worker_thread>(this, i, strategy);
        }
    }

    ~scheduler() {
        if (running_.load(std::memory_order_relaxed)) {
            // Destructor must not block — fall back to immediate shutdown.
            // Users wanting drain-on-exit should call shutdown() explicitly.
            shutdown_force();
        }
        // Clean up exception handler
        delete unhandled_exception_handler_.exchange(nullptr, std::memory_order_acq_rel);
    }

    scheduler(const scheduler&) = delete;
    scheduler& operator=(const scheduler&) = delete;
    scheduler(scheduler&&) = delete;
    scheduler& operator=(scheduler&&) = delete;

    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return;
        }
        
        size_t n = num_threads_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) {
            workers_[i]->start();
        }
        current_scheduler_ = this;
    }

    /// Graceful shutdown: wait for tasks spawned via go/go_to/go_joinable
    /// (and elio::run()) to complete, including those suspended on I/O, then
    /// stop workers. If the timeout elapses with tasks still in flight, the
    /// remaining work is force-stopped (in-flight I/O may be orphaned).
    ///
    /// @param timeout Maximum wait. Default: wait forever.
    /// @return true if all tracked tasks completed within the timeout.
    /// @note Must NOT be called from a worker thread (would deadlock).
    ///       If detected, falls back to shutdown_force() with a warning.
    bool shutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) {
        if (!running_.load(std::memory_order_acquire)) {
            return true;
        }

        bool drained = wait_for_idle(timeout);
        shutdown_force();
        return drained;
    }

    /// Force shutdown: stop workers immediately. Tasks suspended on I/O will
    /// be orphaned (their CQEs are lost, and their frames are destroyed via
    /// drain_remaining_tasks). Use shutdown() for graceful drain.
    void shutdown_force() {
        // Drain the blocking pool BEFORE flipping running_=false. Pool tasks
        // finishing here resume their callers via the spawn_blocking awaitable;
        // that path checks scheduler::is_running() to decide whether to route
        // through scheduler::spawn() (correct) or fall back to caller.resume()
        // on the pool thread (broken — leaves the coroutine without a worker /
        // io_context context, breaking subsequent co_awaits). Doing the drain
        // first keeps running_=true while pool tasks finish, so all resumes
        // land on a real worker.
        //
        // blocking_pool::shutdown() is idempotent, so this is safe even on
        // repeated calls or when shutdown_force() is invoked from contexts
        // where the CAS below will lose.
        if (blocking_pool_) {
            blocking_pool_->shutdown();
        }

        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;
        }

        // Stop all workers (sets running_=false and joins threads).
        // Iterate the visible range; slots beyond num_threads_ are either
        // unpopulated (nullptr) or correspond to workers already stopped by
        // a prior shrink (whose tasks were redistributed at shrink time).
        // Their unique_ptr destructors will run on scheduler destruction and
        // call stop()/drain via ~worker_thread(), which is idempotent.
        size_t n = num_threads_.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i) {
            workers_[i]->stop();
        }

        // Now that ALL workers are stopped, drain remaining tasks
        // This is safe because no worker can steal from another at this point
        for (size_t i = 0; i < n; ++i) {
            workers_[i]->drain_remaining_tasks();
        }

        // Stop and join any draining workers left from prior shrink operations.
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            for (auto& [w, t] : draining_workers_) {
                w->stop();
                if (t.joinable()) t.join();
            }
            draining_workers_.clear();
        }

        // Wake any threads still parked in wait_for_idle so they can observe
        // the !running_ state and bail out promptly.
        {
            std::lock_guard<std::mutex> lock(idle_mutex_);
            idle_cv_.notify_all();
        }

        if (current_scheduler_ == this) {
            current_scheduler_ = nullptr;
        }
    }

    /// Number of tracked tasks currently in flight: spawned but not yet
    /// completed (running, suspended on I/O, sleeping, or queued). Tasks
    /// spawned via raw spawn(handle) are NOT counted; pending I/O operations
    /// from any source ARE counted, so a coroutine waiting on an io_uring
    /// completion always shows up here.
    [[nodiscard]] size_t active_tasks() const noexcept {
        size_t total = active_tracked_.load(std::memory_order_acquire);
        size_t n = num_threads_.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i) {
            total += workers_[i]->queue_size();
            total += workers_[i]->io_context().pending_count();
        }
        return total;
    }

    /// Block until active_tasks() returns 0 or timeout expires. Workers
    /// continue running normally during the wait.
    ///
    /// @param timeout Maximum wait. Default: wait forever.
    /// @return true if drained, false on timeout.
    /// @note Must NOT be called from a worker thread (would deadlock).
    bool wait_for_idle(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) {
        if (worker_thread::current() != nullptr) [[unlikely]] {
            ELIO_LOG_WARNING(
                "wait_for_idle() called from a worker thread; returning false to avoid deadlock");
            return active_tasks() == 0;
        }

        if (timeout <= std::chrono::milliseconds::zero()) {
            return active_tasks() == 0;
        }

        const bool wait_forever = (timeout == std::chrono::milliseconds::max());
        const auto deadline = wait_forever
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() + timeout;

        // Periodic wake to re-evaluate queue/io counters (which don't notify
        // the CV directly). The CV only fires on tracked-task transitions to
        // zero; this poll catches everything else within a bounded window.
        constexpr auto poll_interval = std::chrono::milliseconds(5);

        waiters_.fetch_add(1, std::memory_order_acq_rel);
        std::unique_lock<std::mutex> lock(idle_mutex_);
        while (active_tasks() > 0 && running_.load(std::memory_order_acquire)) {
            auto now = std::chrono::steady_clock::now();
            if (!wait_forever && now >= deadline) break;

            auto wait_for = poll_interval;
            if (!wait_forever) {
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now);
                if (remaining < wait_for) wait_for = remaining;
            }
            idle_cv_.wait_for(lock, wait_for);
        }
        waiters_.fetch_sub(1, std::memory_order_acq_rel);
        return active_tasks() == 0;
    }

    /// Internal: increment tracked-task counter. Called by task_lifecycle_guard
    /// at the top of every wrapper coroutine body.
    void on_task_spawned() noexcept {
        active_tracked_.fetch_add(1, std::memory_order_acq_rel);
    }

    /// Internal: decrement tracked-task counter. Called by task_lifecycle_guard
    /// when the wrapper body exits (normal or exceptional). Notifies waiters
    /// when the counter transitions to zero.
    void on_task_completed() noexcept {
        if (active_tracked_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (waiters_.load(std::memory_order_acquire) > 0) {
                std::lock_guard<std::mutex> lock(idle_mutex_);
                idle_cv_.notify_all();
            }
        }
    }

    void pause() { paused_.store(true, std::memory_order_relaxed); }
    void resume() { paused_.store(false, std::memory_order_relaxed); }

    void spawn(std::coroutine_handle<> handle) {
        if (!handle) [[unlikely]] return;
        if (!running_.load(std::memory_order_relaxed)) [[unlikely]] {
            handle.destroy();
            return;
        }
        // Detach from current thread's frame chain before spawning to another thread
        // to avoid use-after-free when this thread creates another coroutine.
        auto* promise = coro::get_promise_base(handle.address());
        if (promise) {
            promise->detach_from_parent();
        }
        do_spawn(handle);
    }
    
    /// Spawn a task directly (convenience overload)
    /// Accepts any type with a release() method that returns a coroutine_handle
    template<typename Task>
        requires requires(Task t) { { t.release() } -> std::convertible_to<std::coroutine_handle<>>; }
    void spawn(Task&& t) {
        spawn(std::forward<Task>(t).release());
    }

    /// High-level API: fire-and-forget, spawn to this scheduler
    template<typename F, typename... Args>
        requires (std::invocable<F, Args...> && detail::is_task_v<std::invoke_result_t<F, Args...>>)
    void go(F&& f, Args&&... args) {
        do_go_<false, false>(0, std::forward<F>(f), std::forward<Args>(args)...);
    }

    /// High-level API: fire-and-forget, pinned to a specific worker.
    /// Affinity is set so the task cannot be stolen by other workers;
    /// if stolen it will be bounced back to the target worker.
    template<typename F, typename... Args>
        requires (std::invocable<F, Args...> && detail::is_task_v<std::invoke_result_t<F, Args...>>)
    void go_to(size_t worker_id, F&& f, Args&&... args) {
        do_go_<false, true>(worker_id, std::forward<F>(f), std::forward<Args>(args)...);
    }

    /// High-level API: spawn + join, spawn to this scheduler
    template<typename F, typename... Args>
        requires (std::invocable<F, Args...> && detail::is_task_v<std::invoke_result_t<F, Args...>>)
    auto go_joinable(F&& f, Args&&... args)
        -> coro::join_handle<detail::task_value_t<std::invoke_result_t<F, Args...>>>
    {
        return do_go_<true, false>(0, std::forward<F>(f), std::forward<Args>(args)...);
    }

    /// High-level API: spawn + join, pinned to a specific worker.
    /// Affinity is set so the task cannot be stolen; I/O is bound to that
    /// worker's io_context. Use when spawned task and caller must share fate
    /// w.r.t. thread-pool resizing.
    template<typename F, typename... Args>
        requires (std::invocable<F, Args...> && detail::is_task_v<std::invoke_result_t<F, Args...>>)
    auto go_joinable_to(size_t worker_id, F&& f, Args&&... args)
        -> coro::join_handle<detail::task_value_t<std::invoke_result_t<F, Args...>>>
    {
        return do_go_<true, true>(worker_id, std::forward<F>(f), std::forward<Args>(args)...);
    }

    void spawn_to(size_t worker_id, std::coroutine_handle<> handle) {
        if (!handle) [[unlikely]] return;
        if (!running_.load(std::memory_order_relaxed)) [[unlikely]] {
            handle.destroy();
            return;
        }

        // Detach from current thread's frame chain before spawning to another thread
        // to avoid use-after-free when this thread creates another coroutine.
        auto* promise = coro::get_promise_base(handle.address());
        if (promise) {
            promise->detach_from_parent();
        }

        size_t n = num_threads_.load(std::memory_order_acquire);
        if (n == 0) [[unlikely]] {
            handle.destroy();
            return;
        }
        size_t idx = worker_id % n;
        // Verify the chosen slot is still running. After a shrink the worker at
        // this index may have been stopped (or be in the process of being
        // stopped) — pushing into its inbox would orphan the task. Fall through
        // to do_spawn() so the round-robin path picks a worker that is still
        // accepting work.
        if (workers_[idx] && workers_[idx]->is_running()) {
            workers_[idx]->schedule(handle);
            return;
        }
        do_spawn(handle);
    }

    [[nodiscard]] size_t num_threads(std::memory_order order = std::memory_order_relaxed) const noexcept {
        return num_threads_.load(order);
    }

    [[nodiscard]] size_t pending_tasks() const noexcept {
        size_t n = num_threads_.load(std::memory_order_acquire);
        size_t total = 0;
        for (size_t i = 0; i < n; ++i) {
            total += workers_[i]->queue_size();
            total += workers_[i]->io_context().pending_count();
        }
        return total;
    }

    void set_thread_count(size_t count) {
        // Guard against calling from a worker thread. The shrink path joins
        // worker threads while holding workers_mutex_; if the joined worker's
        // current task calls set_thread_count() re-entrantly, it would deadlock
        // trying to acquire the same non-recursive mutex.
        if (worker_thread::current() != nullptr) [[unlikely]] {
            ELIO_LOG_WARNING(
                "set_thread_count() called from a worker thread; ignoring to avoid deadlock");
            return;
        }

        if (count == 0) count = 1;
        if (count > MAX_THREADS) count = MAX_THREADS;
        
        size_t old_count = num_threads_.load(std::memory_order_relaxed);
        
        if (count > old_count) {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            old_count = num_threads_.load(std::memory_order_relaxed);
            if (count <= old_count) return;
            
            for (size_t i = old_count; i < count; ++i) {
                if (workers_[i]) {
                    // Slot already populated (either by ctor or a prior grow
                    // that was later shrunk). Restart the worker in place.
                    if (running_.load(std::memory_order_relaxed)) {
                        workers_[i]->start();
                    }
                } else {
                    auto worker = std::make_unique<worker_thread>(this, i, wait_strategy_);
                    if (running_.load(std::memory_order_relaxed)) {
                        worker->start();
                    }
                    workers_[i] = std::move(worker);
                }
            }
            // Publish the new size AFTER all slot writes are visible. Hot-path
            // readers (get_worker, do_spawn, active_tasks) gate on
            // num_threads_.load(acquire); the release here pairs with their
            // acquire to make the slot writes visible.
            num_threads_.store(count, std::memory_order_release);
        } else if (count < old_count) {
            // Hold the lock across the entire shrink — including I/O drain,
            // stop(), and redistribute_tasks(). This serializes shrink against
            // a concurrent grow: without it, a grow that runs in the window
            // between the num_threads_ store and the stop() calls would
            // observe doomed workers as still "running" (because stop() hasn't
            // run yet) and republish num_threads_ to a value that re-exposes
            // them — only for shrink to then stop them, leaving slots inside
            // the visible range backed by stopped workers and routing new
            // spawns into stopped inboxes.
            //
            // stop() joins the worker thread which can take a few ticks, but
            // bounded by the worker's poll timeout and any in-flight task.
            // Correctness wins over throughput here: set_thread_count is a
            // slow-path control operation, not a hot path.
            //
            // Note: must NOT be called from a worker thread — joining the
            // current thread would deadlock. Same caveat as before this fix.
            std::lock_guard<std::mutex> lock(workers_mutex_);
            old_count = num_threads_.load(std::memory_order_relaxed);
            if (count >= old_count) return;

            // Update count first so new spawns go to remaining workers.
            num_threads_.store(count, std::memory_order_release);

            // Wait for the doomed workers' I/O contexts to drain before stopping
            // them. If a worker is stopped while it has pending I/O (e.g. a
            // coroutine suspended on co_await recv()), the io_context is no
            // longer polled and those coroutines never resume — silent leak.
            constexpr auto io_drain_timeout = std::chrono::seconds(5);
            constexpr auto poll_interval = std::chrono::milliseconds(1);
            auto deadline = std::chrono::steady_clock::now() + io_drain_timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                bool all_drained = true;
                for (size_t i = count; i < old_count; ++i) {
                    if (workers_[i]->io_context().pending_count() > 0) {
                        all_drained = false;
                        break;
                    }
                }
                if (all_drained) break;
                std::this_thread::sleep_for(poll_interval);
            }

            // Join any previously-draining workers that have finished.
            reap_draining_workers_();

            for (size_t i = count; i < old_count; ++i) {
                if (workers_[i]->io_context().pending_count() == 0) {
                    // Fully drained — stop immediately.
                    workers_[i]->stop();
                    workers_[i]->redistribute_tasks(this);
                } else {
                    // Still has pending I/O — enter draining mode. The worker
                    // continues polling its io_context and self-exits when
                    // pending_count() reaches 0.
                    workers_[i]->enter_draining_mode();
                    std::thread t = workers_[i]->detach_thread();
                    draining_workers_.emplace_back(std::move(workers_[i]), std::move(t));
                }
            }
        }
    }

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool is_paused() const noexcept {
        return paused_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static scheduler* current() noexcept {
        return current_scheduler_;
    }

    [[nodiscard]] worker_thread* get_worker(size_t index) {
        if (index < num_threads_.load(std::memory_order_acquire)) {
            return workers_[index].get();
        }
        return nullptr;
    }

    [[nodiscard]] size_t total_tasks_executed() const noexcept {
        size_t n = num_threads_.load(std::memory_order_acquire);
        size_t total = 0;
        for (size_t i = 0; i < n; ++i) {
            total += workers_[i]->tasks_executed();
        }
        return total;
    }

    [[nodiscard]] size_t worker_tasks_executed(size_t worker_id) const noexcept {
        size_t n = num_threads_.load(std::memory_order_acquire);
        if (worker_id >= n) return 0;
        return workers_[worker_id]->tasks_executed();
    }

    [[nodiscard]] size_t total_steals_executed() const noexcept {
        size_t n = num_threads_.load(std::memory_order_acquire);
        size_t total = 0;
        for (size_t i = 0; i < n; ++i) {
            total += workers_[i]->steals_executed();
        }
        return total;
    }

    [[nodiscard]] size_t worker_steals_executed(size_t worker_id) const noexcept {
        size_t n = num_threads_.load(std::memory_order_acquire);
        if (worker_id >= n) return 0;
        return workers_[worker_id]->steals_executed();
    }

    /// Get the wait strategy used by this scheduler
    [[nodiscard]] const wait_strategy& get_wait_strategy() const noexcept {
        return wait_strategy_;
    }

    /// Get the blocking pool for spawn_blocking operations
    [[nodiscard]] blocking_pool* get_blocking_pool() noexcept {
        return blocking_pool_.get();
    }

    /// Exception handler type for unhandled exceptions in detached tasks
    /// and when_any loser exceptions.
    using unhandled_exception_handler = std::function<void(std::exception_ptr)>;

    /// Set the per-scheduler unhandled exception handler.
    /// Called when:
    /// - A detached task (go/go_to) throws and the exception is not observed
    /// - A when_any loser throws after the winner has already resolved
    ///
    /// Default behavior (no handler set): log ERROR with exception info.
    /// When handler is set: invoke handler instead of logging.
    ///
    /// Thread-safe: handler pointer is atomic. Can be set from any thread.
    void set_unhandled_exception_handler(unhandled_exception_handler handler) {
        auto* new_handler = handler
            ? new unhandled_exception_handler(std::move(handler))
            : nullptr;
        auto* old_handler = unhandled_exception_handler_.exchange(
            new_handler, std::memory_order_acq_rel);
        delete old_handler;
    }

    /// Get the current unhandled exception handler (may be nullptr).
    [[nodiscard]] const unhandled_exception_handler* get_unhandled_exception_handler() const noexcept {
        return unhandled_exception_handler_.load(std::memory_order_acquire);
    }

    /// Report an unhandled exception. If handler is set, invoke it.
    /// Otherwise log ERROR.
    void report_unhandled_exception(std::exception_ptr ex) noexcept {
        if (!ex) return;
        auto* handler = unhandled_exception_handler_.load(std::memory_order_acquire);
        if (handler) {
            try {
                (*handler)(std::move(ex));
            } catch (...) {
                // Handler itself threw — log both
                ELIO_LOG_ERROR("unhandled exception handler threw; original exception dropped");
            }
        } else {
            try {
                std::rethrow_exception(ex);
            } catch (const std::exception& e) {
                ELIO_LOG_ERROR("unhandled exception in detached task or when_any loser: {}", e.what());
            } catch (...) {
                ELIO_LOG_ERROR("unhandled exception in detached task or when_any loser: <unknown>");
            }
        }
    }

private:
    /// Pair an ``on_task_spawned()`` increment with a deferred decrement
    /// installed on the wrapper coroutine's promise. The decrement fires
    /// from ``promise_base::~promise_base`` whenever the frame is freed —
    /// normal completion, forced ``handle.destroy()`` mid-flight, or
    /// drain during shutdown — so the counter is balanced on every path.
    /// Must be called while the wrapper handle is still live and before
    /// ``do_spawn`` (or any branch that may destroy the handle).
    /// noexcept on every line so there is no throw-between-+1-and-callback
    /// window.
    void mark_tracked_(coro::promise_base& p) noexcept {
        on_task_spawned();
        p.on_spawn_completion_data_ = this;
        p.on_spawn_completion_ = +[](void* self) noexcept {
            static_cast<scheduler*>(self)->on_task_completed();
        };
    }

    template<bool Joinable, bool Pinned, typename F, typename... Args>
    auto do_go_(size_t worker_id, F&& f, Args&&... args) {
        using ResultTask = std::invoke_result_t<F, Args...>;
        using T = detail::task_value_t<ResultTask>;

        auto vstack_owner = std::make_unique<coro::vthread_stack>();
        auto* new_vstack = vstack_owner.get();
        auto* old_vstack = coro::vthread_stack::current();
        auto* old_frame = coro::promise_base::current_frame();
        coro::vthread_stack::set_current(new_vstack);

        auto wrapper = [&] {
            if constexpr (Joinable) {
                return detail::callable_wrapper(std::forward<F>(f), std::forward<Args>(args)...);
            } else {
                return detail::callable_wrapper_void(std::forward<F>(f), std::forward<Args>(args)...);
            }
        }();

        coro::vthread_stack::set_current(old_vstack);

        auto handle = coro::detail::task_access::release(wrapper);
        handle.promise().detached_ = true;
        handle.promise().set_vstack_owner(vstack_owner.release());
        if constexpr (Pinned) {
            handle.promise().set_affinity(worker_id);
        }
        handle.promise().detach_from_parent();
        // Restore caller's frame chain. detach_from_parent() sets current_frame_
        // to nullptr to avoid UAF when parent_ was spawned to another thread,
        // but in do_go_ the parent is the caller on the same thread and is safe.
        // Without this restore, the caller's subsequent coroutines would have
        // nullptr as parent_, breaking the virtual stack chain.
        coro::promise_base::set_current_frame(old_frame);

        if constexpr (Joinable) {
            auto state = std::make_shared<coro::detail::join_state<T>>();
            handle.promise().join_state_ = state;
            mark_tracked_(handle.promise());
            if constexpr (Pinned) {
                spawn_to(worker_id, handle);
            } else {
                do_spawn(handle);
            }
            return coro::join_handle<T>(std::move(state));
        } else {
            mark_tracked_(handle.promise());
            if constexpr (Pinned) {
                spawn_to(worker_id, handle);
            } else {
                do_spawn(handle);
            }
        }
    }

    void reap_draining_workers_() noexcept {
        draining_workers_.erase(
            std::remove_if(draining_workers_.begin(), draining_workers_.end(),
                [](auto& entry) {
                    auto& [w, t] = entry;
                    if (!w->is_running()) {
                        if (t.joinable()) t.join();
                        return true;
                    }
                    return false;
                }),
            draining_workers_.end());
    }

    void do_spawn(std::coroutine_handle<> handle) {
        // Release fence ensures all writes to the coroutine frame (including
        // captured lambda state) are visible to the worker that will run this task
        std::atomic_thread_fence(std::memory_order_release);

        size_t n = num_threads_.load(std::memory_order_acquire);
        if (n == 0) [[unlikely]] {
            handle.destroy();
            return;
        }

        // Check if task has affinity - if so, schedule to that specific worker
        size_t affinity = coro::get_affinity(handle.address());
        if (affinity != coro::NO_AFFINITY && affinity < n) {
            if (workers_[affinity]->is_running()) {
                workers_[affinity]->schedule(handle);
                return;
            }
            // Target worker not running - clear affinity and fall through
            auto* promise = coro::get_promise_base(handle.address());
            if (promise) {
                promise->clear_affinity();
            }
        }

        // No affinity or invalid affinity - round-robin to any running worker
        size_t start_index = spawn_index_.fetch_add(1, std::memory_order_relaxed) % n;
        for (size_t i = 0; i < n; ++i) {
            size_t index = (start_index + i) % n;
            if (workers_[index]->is_running()) {
                workers_[index]->schedule(handle);
                return;
            }
        }

        // All workers stopped, try again with current thread count
        n = num_threads_.load(std::memory_order_acquire);
        if (n > 0 && workers_[0]->is_running()) {
            workers_[0]->schedule(handle);
        } else {
            handle.destroy();
        }
    }

    // Fixed-size storage indexed up to MAX_THREADS. Slot occupancy is
    // controlled by the num_threads_ atomic: writers populate workers_[i]
    // before publishing the new size via num_threads_.store(release);
    // readers gate on num_threads_.load(acquire), so the release/acquire
    // pair makes the slot pointer visible. Using std::array (rather than
    // std::vector + reserve) eliminates the implicit size-field write that
    // vector::push_back performs even when no reallocation occurs — that
    // write was the source of a TSAN race against lock-free hot-path
    // readers (get_worker, schedule_round_robin, active_tasks, ...).
    std::array<std::unique_ptr<worker_thread>, MAX_THREADS> workers_;

    // Frequently-read fields on their own cache line to avoid false sharing
    // with the spawn counter and the slow-path workers_mutex_.
    alignas(64) std::atomic<size_t> num_threads_;
    std::atomic<bool> running_;
    std::atomic<bool> paused_;

    // spawn_index_ is incremented on every spawn(); isolate it so modifications
    // don't invalidate the num_threads_/running_ cache line on other cores.
    alignas(64) std::atomic<size_t> spawn_index_;

    // workers_mutex_ is only touched on slow-path resize operations;
    // keep it away from the hot read fields above.
    alignas(64) mutable std::mutex workers_mutex_;
    wait_strategy wait_strategy_;

    // Blocking pool for spawn_blocking operations
    std::unique_ptr<blocking_pool> blocking_pool_;

    // Tracked-task accounting for graceful shutdown / wait_for_idle.
    // active_tracked_ is incremented by every wrapper coroutine on first
    // resume and decremented when the wrapper's body exits. waiters_ is
    // a hint so the on-completion path skips the CV mutex when no one is
    // parked in wait_for_idle.
    alignas(64) std::atomic<size_t> active_tracked_{0};
    alignas(64) std::atomic<size_t> waiters_{0};
    mutable std::mutex idle_mutex_;
    mutable std::condition_variable idle_cv_;

    // Workers that are draining pending I/O after a shrink operation.
    // Protected by workers_mutex_.
    std::vector<std::pair<std::unique_ptr<worker_thread>, std::thread>> draining_workers_;

    // Per-scheduler unhandled exception handler. Nullptr means default behavior
    // (log ERROR). Owned via raw pointer + atomic for lock-free reads on hot path.
    // Set/delete via set_unhandled_exception_handler().
    std::atomic<unhandled_exception_handler*> unhandled_exception_handler_{nullptr};

    static inline thread_local scheduler* current_scheduler_ = nullptr;
};

} // namespace elio::runtime

namespace elio::runtime {

inline scheduler* get_current_scheduler() noexcept {
    return scheduler::current();
}

inline void schedule_handle(std::coroutine_handle<> handle) noexcept {
    if (!handle) return;

    auto* sched = scheduler::current();
    if (sched && sched->is_running()) {
        sched->spawn(handle);
    } else {
        static thread_local std::vector<std::coroutine_handle<>> trampoline_queue;
        static thread_local bool trampoline_running = false;

        trampoline_queue.push_back(handle);

        if (!trampoline_running) {
            trampoline_running = true;
            struct trampoline_guard {
                std::vector<std::coroutine_handle<>>& q;
                ~trampoline_guard() {
                    for (auto h : q) {
                        if (!h.done()) h.destroy();
                    }
                    q.clear();
                    trampoline_running = false;
                }
            } guard{trampoline_queue};
            while (!trampoline_queue.empty()) {
                auto h = trampoline_queue.back();
                trampoline_queue.pop_back();
                if (!h.done()) h.resume();
            }
        }
    }
}

inline void report_detached_exception(std::exception_ptr ex) noexcept {
    if (!ex) return;
    auto* sched = scheduler::current();
    if (sched) {
        sched->report_unhandled_exception(std::move(ex));
    } else {
        // No scheduler context — log directly
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            ELIO_LOG_ERROR("unhandled exception in detached (go) task: {}", e.what());
        } catch (...) {
            ELIO_LOG_ERROR("unhandled exception in detached (go) task: <unknown>");
        }
    }
}

inline void worker_thread::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    thread_ = std::thread(&worker_thread::run, this);
}

inline void worker_thread::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false, 
            std::memory_order_release, std::memory_order_relaxed)) return;
    wake();  // Wake the worker if it's blocked in I/O poll
    if (thread_.joinable()) thread_.join();
}

/// Final cleanup for any orphaned tasks - only call after ALL workers have stopped.
/// This is a safety net for edge cases where tasks might still exist after drain phase.
inline void worker_thread::drain_remaining_tasks() noexcept {
    // First drain inbox to deque
    void* addr;
    while ((addr = inbox_->pop()) != nullptr) {
        queue_->push(addr);
    }
    // Destroy any remaining tasks (should be rare after drain phase in run())
    while ((addr = queue_->pop()) != nullptr) {
        auto handle = std::coroutine_handle<>::from_address(addr);
        if (handle) {
            handle.destroy();
        }
    }
}

/// Redistribute remaining tasks to active workers - call during thread pool shrink
inline void worker_thread::redistribute_tasks(scheduler* sched) noexcept {
    // First drain inbox to deque
    void* addr;
    while ((addr = inbox_->pop()) != nullptr) {
        queue_->push(addr);
    }
    // Then redistribute all tasks to active workers
    while ((addr = queue_->pop()) != nullptr) {
        auto handle = std::coroutine_handle<>::from_address(addr);
        if (handle && !handle.done()) {
            // Respawn to an active worker
            sched->spawn(handle);
        } else if (handle) {
            handle.destroy();
        }
    }
}

inline void worker_thread::drain_inbox() noexcept {
    // Drain MPSC inbox into local Chase-Lev deque
    // Drain all available items to ensure tasks aren't stuck in inbox
    void* item;
    while ((item = inbox_->pop()) != nullptr) {
        queue_->push(item);
    }
}

inline void worker_thread::run() {
    // Block common signals on this worker thread so they are delivered via
    // signalfd rather than invoking the default disposition (which may
    // terminate the process).  Threads inherit the signal mask of their
    // parent, but the main thread may not have blocked these before
    // spawning workers.  Blocking here is idempotent and safe.
    {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    }

    // Set the current scheduler and worker for this thread
    scheduler::current_scheduler_ = scheduler_;
    current_worker_ = this;

    while (running_.load(std::memory_order_relaxed)) {
        if (draining_.load(std::memory_order_acquire)) [[unlikely]] {
            // Draining mode: only poll I/O, no new tasks or stealing.
            // Redistribute any queued tasks, then wait for pending I/O to complete.
            redistribute_tasks(scheduler_);
            if (io_context_->pending_count() == 0 ||
                std::chrono::steady_clock::now() >= draining_deadline_) {
                running_.store(false, std::memory_order_release);
                break;
            }
            io_context_->poll(std::chrono::milliseconds(50));
            continue;
        }

        if (scheduler_->is_paused()) [[unlikely]] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        auto handle = get_next_task();

        if (handle) {
            run_task(handle);
        } else {
            poll_io_when_idle();
        }
    }
    
    // Drain phase: after running_ becomes false, continue executing all
    // remaining tasks until both local queue and inbox are empty.
    // This ensures shutdown() returns only when all submitted tasks have
    // fully completed (including coroutine cleanup and lambda destruction).
    //
    // Note: pop() (steal-style CAS path) — NOT pop_local(false). Other
    // workers may still be inside try_steal()->steal_task() against this
    // worker's deque because they haven't yet observed our running_=false.
    // Chase-Lev requires either single-threaded owner pop OR concurrent
    // steal+pop synchronized via the seq_cst fence inside pop(); the
    // single-thread fast path of pop_local() races with those stealers.
    while (true) {
        drain_inbox();
        void* addr = queue_->pop();
        if (!addr) break;

        auto handle = std::coroutine_handle<>::from_address(addr);
        if (handle && !handle.done()) {
            needs_sync_ = true;  // Conservatively ensure memory visibility for drained tasks
            run_task(handle);
        }
    }
    
    // Clear the references when done
    scheduler::current_scheduler_ = nullptr;
    current_worker_ = nullptr;
}

inline std::coroutine_handle<> worker_thread::get_next_task() noexcept {
    // Always pass allow_concurrent_steals=true to pop_local(). The thread count
    // can change at runtime via set_thread_count(), and any TOCTOU window between
    // a num_threads() check and the pop_local() call could let a newly-started
    // worker begin stealing while we use the unsafe fast path (no seq_cst fence).
    // The cost of always using the safe pop() path is one seq_cst fence per task,
    // which is negligible compared to coroutine resume overhead.
    constexpr bool allow_concurrent_steals = true;

    // Fast path: pop from local deque first
    void* addr = queue_->pop_local(allow_concurrent_steals);
    if (addr) {
        needs_sync_ = false;  // Local task, no sync needed
        return std::coroutine_handle<>::from_address(addr);
    }

    // Local queue empty - drain any externally submitted tasks from inbox
    drain_inbox();

    // Try local deque again after draining inbox
    addr = queue_->pop_local(allow_concurrent_steals);
    if (addr) {
        needs_sync_ = true;  // Came from inbox, needs sync
        return std::coroutine_handle<>::from_address(addr);
    }

    // Nothing local, try stealing from other workers.
    // Use acquire ordering to synchronize with set_thread_count()'s release
    // store, ensuring we see newly-added workers before deciding to skip steal.
    bool single_worker = (scheduler_->num_threads(std::memory_order_acquire) == 1);
    if (!single_worker) {
        auto handle = try_steal();
        if (handle) {
            needs_sync_ = true;  // Stolen task, needs sync
        }
        return handle;
    }
    
    return nullptr;
}

inline void worker_thread::run_task(std::coroutine_handle<> handle) noexcept {
    // Acquire fence only for tasks from external sources (inbox/steal)
    // Local tasks don't need synchronization - same thread visibility
    if (needs_sync_) {
        std::atomic_thread_fence(std::memory_order_acquire);
    }
    
    if (!handle || handle.done()) [[unlikely]] return;

    // Context switch: set vstack and current_frame before resume, restore after
    auto* promise = coro::get_promise_base(handle.address());
    auto* prev_vstack = coro::vthread_stack::current();
    auto* prev_frame = coro::promise_base::current_frame();
    if (promise) {
        coro::vthread_stack::set_current(promise->vstack());
        coro::promise_base::set_current_frame(promise);
    }

    handle.resume();

    coro::vthread_stack::set_current(prev_vstack);
    coro::promise_base::set_current_frame(prev_frame);
    tasks_executed_.fetch_add(1, std::memory_order_relaxed);
    update_last_task_time();

    // Note: We do NOT check done() or call destroy() here.
    // If the task completed, its final_suspend will self-destruct (fire-and-forget)
    // or resume a continuation. If it suspended mid-execution (e.g., yield),
    // another thread may already be running it - touching the handle would race.
}

inline std::coroutine_handle<> worker_thread::try_steal() noexcept {
    size_t num_workers = scheduler_->num_threads();
    if (num_workers <= 1) return nullptr;
    
    // Start stealing from a different worker each time (better distribution)
    thread_local size_t steal_start = 0;
    size_t start = steal_start;
    steal_start = (steal_start + 1) % num_workers;
    
    // Limit retries to avoid infinite loops when all tasks have affinity
    constexpr size_t max_retries = 8;
    size_t retry_count = 0;
    
    for (size_t i = 0; i < num_workers; ++i) {
        size_t victim_id = (start + i) % num_workers;
        if (victim_id == worker_id_) continue;
        
        auto* victim = scheduler_->get_worker(victim_id);
        if (!victim || !victim->is_running()) continue;
        
        // Try single steal - batch stealing has race conditions with owner's pop
        auto handle = victim->steal_task();
        if (handle) {
            // Check if this task has affinity for a different worker
            size_t affinity = coro::get_affinity(handle.address());
            
            if (affinity == coro::NO_AFFINITY || affinity == worker_id_) {
                steals_executed_.fetch_add(1, std::memory_order_relaxed);
                return handle;
            }
            
            // Task has affinity for another worker - schedule it there
            if (affinity < num_workers) {
                scheduler_->spawn_to(affinity, handle);
            } else {
                auto* promise = coro::get_promise_base(handle.address());
                if (promise) {
                    promise->clear_affinity();
                }
                steals_executed_.fetch_add(1, std::memory_order_relaxed);
                return handle;
            }
            
            // Continue trying to steal, but limit retries
            if (++retry_count >= max_retries) {
                return nullptr;
            }
            // Restart the search from a different victim
            i = static_cast<size_t>(-1);  // Will be 0 after increment
            start = (steal_start + retry_count) % num_workers;
        }
    }
    
    return nullptr;
}

inline void worker_thread::poll_io_when_idle() {
    constexpr int idle_timeout_ms = 10;

    // Mark as idle before any blocking - enables lazy wake optimization
    idle_.store(true, std::memory_order_release);

    // Optional spinning phase (if configured via wait_strategy)
    if (strategy_.spin_iterations > 0) {
        for (size_t i = 0; i < strategy_.spin_iterations; ++i) {
            if (inbox_->size_approx() > 0) {
                idle_.store(false, std::memory_order_relaxed);
                return;
            }
            if (strategy_.spin_yield) {
                std::this_thread::yield();
            } else {
                cpu_relax();
            }
        }
    }

    // Single unified wait: blocks on I/O backend (epoll/io_uring)
    // Both I/O completions AND task wake-ups (via eventfd) will unblock this
    io_context_->poll(std::chrono::milliseconds(idle_timeout_ms));

    // Clear idle flag after waking up
    idle_.store(false, std::memory_order_relaxed);
}

} // namespace elio::runtime
