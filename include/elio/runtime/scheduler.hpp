#pragma once

#include "worker_thread.hpp"
#include "blocking_pool.hpp"
#include <elio/log/macros.hpp>
#include <elio/coro/frame.hpp>
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
#include <exception>
#include <stdexcept>
#include <vector>

namespace elio::coro {
class task_group;
}

namespace elio::runtime {

class scheduler;

namespace detail {
    using elio::detail::task_value_t;
    using elio::detail::is_task;
    using elio::detail::is_task_v;

#ifdef ELIO_RUNTIME_TEST_HOOKS
    inline std::atomic<bool> reject_next_schedule_for_test{false};
    inline std::atomic<bool> reject_next_spawn_for_test{false};
    inline std::atomic<size_t> local_schedule_fallbacks_for_test{0};
    inline std::atomic<bool> pause_shutdown_teardown_for_test{false};
    inline std::atomic<bool> shutdown_teardown_paused_for_test{false};
    inline std::atomic<size_t> shutdown_waiters_for_test{0};
    inline std::atomic<bool> resize_waiting_for_draining_worker_for_test{false};
    inline std::atomic<bool> hold_draining_worker_for_test{false};
    inline std::atomic<bool> draining_worker_held_for_test{false};
#endif

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
    friend class elio::coro::task_group;
    friend void schedule_handle(std::coroutine_handle<> handle) noexcept;
    
public:
    static constexpr size_t MAX_THREADS = 256;

    explicit scheduler(size_t num_threads = std::thread::hardware_concurrency(),
                       wait_strategy strategy = wait_strategy::blocking(),
                       size_t blocking_threads = 4)
        : num_threads_(std::min(num_threads == 0 ? size_t{1} : num_threads, MAX_THREADS))
        , running_(false)
        , paused_(false)
        , spawn_index_(0)
        , wait_strategy_(strategy)
        , blocking_pool_(std::make_unique<blocking_pool>(
              // Scheduler-owned pools must be joinable so shutdown_force() can
              // drain accepted blocking work before tearing down scheduler state.
              // Normalize 0 → 1 here; standalone blocking_pool(0) still uses
              // detached per-task threads.
              blocking_threads == 0 ? size_t{1} : blocking_threads)) {

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
        auto* current_worker = worker_thread::current();
        if (current_worker && current_worker->scheduler_ == this) {
            ELIO_LOG_ERROR(
                "scheduler destruction from one of its worker threads is unsupported");
            std::terminate();
        }
        // Also waits for a concurrent worker-initiated teardown winner and
        // joins that worker after its shutdown_force() call unwinds.
        shutdown_force();
        if (current_scheduler_ == this) {
            current_scheduler_ = nullptr;
        }
        unhandled_exception_handler_.store(
            std::shared_ptr<const unhandled_exception_handler>{},
            std::memory_order_release);
    }

    scheduler(const scheduler&) = delete;
    scheduler& operator=(const scheduler&) = delete;
    scheduler(scheduler&&) = delete;
    scheduler& operator=(scheduler&&) = delete;

    void start() {
        std::unique_lock<std::mutex> shutdown_lock(shutdown_mutex_);
        shutdown_cv_.wait(shutdown_lock, [this] {
            return !resize_in_progress_ || shutdown_started_;
        });
        // Scheduler-owned resources, including the blocking pool, have a
        // one-shot lifecycle. In particular, a worker-initiated shutdown may
        // have finished teardown but still be awaiting an external join.
        if (shutdown_started_) return;
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
            // running_=false is published before the teardown winner finishes.
            // External callers must still wait for that phase and join any
            // worker that initiated shutdown itself.
            shutdown_force();
            return true;
        }

        if (worker_thread::current() != nullptr) [[unlikely]] {
            ELIO_LOG_WARNING(
                "shutdown() called from a worker thread; falling back to shutdown_force() to avoid deadlock");
            shutdown_force();
            return active_tasks() == 0;
        }

        bool drained = wait_for_idle(timeout);
        shutdown_force();
        return drained;
    }

    /// Force shutdown: stop scheduler workers without gracefully draining
    /// tracked tasks or pending I/O. Already-accepted scheduler-owned blocking
    /// work is drained first so spawn_blocking continuations can resume on
    /// scheduler workers before teardown. Tasks suspended on I/O will be
    /// orphaned when their CQEs are lost. Use shutdown() for graceful drain.
    /// A scheduler worker cannot join itself. If it requests shutdown while an
    /// external resize is waiting for that worker to drain, teardown is handed
    /// to the resize controller and this call returns before final teardown.
    void shutdown_force() {
        auto* current_worker = worker_thread::current();
        const bool called_from_own_worker =
            current_worker && current_worker->scheduler_ == this;

        while (true) {
            std::unique_lock<std::mutex> lock(shutdown_mutex_);
            if (shutdown_teardown_in_progress_) {
                // The winner may be joining this worker. Waiting here would
                // deadlock that join; let the task unwind instead.
                if (called_from_own_worker) return;
#ifdef ELIO_RUNTIME_TEST_HOOKS
                detail::shutdown_waiters_for_test.fetch_add(
                    1, std::memory_order_release);
                detail::shutdown_waiters_for_test.notify_all();
#endif
                shutdown_cv_.wait(lock, [this] {
                    return !shutdown_teardown_in_progress_;
                });
#ifdef ELIO_RUNTIME_TEST_HOOKS
                detail::shutdown_waiters_for_test.fetch_sub(
                    1, std::memory_order_release);
#endif
                lock.unlock();
                join_remaining_workers_();
                return;
            }

            // Every first shutdown invocation closes the one-shot lifecycle,
            // including shutdown before start().
            shutdown_started_ = true;
            if (resize_in_progress_) {
                // A resize may be joining this draining worker. Waiting from
                // the worker would deadlock that join, so hand teardown to the
                // external resize controller after it releases resize state.
                if (called_from_own_worker) {
                    shutdown_deferred_by_resize_ = true;
                    return;
                }
                shutdown_cv_.wait(lock, [this] {
                    return !resize_in_progress_;
                });
                continue;
            }
            if (!running_.load(std::memory_order_acquire)) {
                lock.unlock();
                if (!called_from_own_worker) {
                    join_remaining_workers_();
                }
                return;
            }
            shutdown_teardown_in_progress_ = true;
            break;
        }

#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (detail::pause_shutdown_teardown_for_test.load(
                std::memory_order_acquire)) {
            detail::shutdown_teardown_paused_for_test.store(
                true, std::memory_order_release);
            detail::shutdown_teardown_paused_for_test.notify_all();
            while (detail::pause_shutdown_teardown_for_test.load(
                    std::memory_order_acquire)) {
                detail::pause_shutdown_teardown_for_test.wait(
                    true, std::memory_order_acquire);
            }
            detail::shutdown_teardown_paused_for_test.store(
                false, std::memory_order_release);
        }
#endif

        // Drain the blocking pool BEFORE flipping running_=false. Pool tasks
        // finishing here resume their callers through
        // scheduler::try_schedule(). Doing the drain first keeps running_=true
        // while accepted work finishes, so every continuation can land on a
        // real worker instead of being stranded during teardown.
        //
        // blocking_pool::shutdown() is idempotent, so this is safe even on
        // repeated calls or when shutdown_force() is invoked from contexts
        // where the CAS below will lose.
        if (blocking_pool_) {
            blocking_pool_->shutdown();
        }

        running_.store(false, std::memory_order_release);

        try {
            perform_shutdown_teardown_(current_worker);
        } catch (...) {
            finish_shutdown_teardown_();
            throw;
        }
        finish_shutdown_teardown_();

        if (!called_from_own_worker) {
            join_remaining_workers_();
        }
    }

private:
    void perform_shutdown_teardown_(worker_thread* current_worker) {

        // Stop all workers (sets running_=false and joins threads). If this is
        // called from a worker, request that worker to stop but let its run()
        // loop unwind after the current task returns; joining it here would
        // self-join.
        // Iterate the visible range; slots beyond num_threads_ are either
        // unpopulated (nullptr) or correspond to workers already stopped by
        // a prior shrink (whose tasks were redistributed at shrink time).
        // Their unique_ptr destructors will run on scheduler destruction and
        // call stop()/drain via ~worker_thread(), which is idempotent.
        size_t n = num_threads_.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i) {
            if (workers_[i].get() == current_worker) {
                workers_[i]->request_stop();
            } else {
                workers_[i]->stop();
            }
        }

        // Now that joined workers are stopped, drain remaining tasks there.
        // The current worker, if any, will run its normal drain phase after
        // this task returns.
        for (size_t i = 0; i < n; ++i) {
            if (workers_[i].get() != current_worker) {
                workers_[i]->drain_remaining_tasks();
            }
        }

        // Stop and join any draining workers left from prior shrink operations.
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            auto it = draining_workers_.begin();
            while (it != draining_workers_.end()) {
                auto& [idx, thread] = *it;
                auto* worker = workers_[idx].get();
                if (worker == current_worker) {
                    worker->request_stop();
                    ++it;
                    continue;
                }
                if (worker) {
                    worker->stop();
                }
                if (thread.joinable()) thread.join();
                if (worker) {
                    worker->leave_draining_mode();
                }
                clear_draining_slot_(idx);
                it = draining_workers_.erase(it);
            }
        }

        // Wake any threads still parked in wait_for_idle so they can observe
        // the !running_ state and bail out promptly.
        {
            std::lock_guard<std::mutex> lock(idle_mutex_);
            idle_cv_.notify_all();
        }

        // A worker that initiated force shutdown still has to finish its
        // current coroutine and drain locally deferred continuations. Keep its
        // scheduler domain visible until worker_thread::run() actually exits.
        if (current_scheduler_ == this && current_worker == nullptr) {
            current_scheduler_ = nullptr;
        }
    }

public:

    /// Number of tracked tasks currently in flight: spawned but not yet
    /// completed (running, suspended on I/O, sleeping, or queued). Tasks
    /// spawned via raw spawn(handle) are NOT counted; pending I/O operations
    /// from any source ARE counted, so a coroutine waiting on an io_uring
    /// completion always shows up here.
    [[nodiscard]] size_t active_tasks() const noexcept {
        size_t total = active_tracked_.load(std::memory_order_acquire);
        size_t n = num_threads_.load(std::memory_order_acquire);
        return total + worker_pending_load_(n);
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

    /// Internal: increment tracked-task counter. Called by mark_tracked_()
    /// before the wrapper handle is exposed to the scheduler.
    void on_task_spawned() noexcept {
        active_tracked_.fetch_add(1, std::memory_order_acq_rel);
    }

    /// Internal: decrement tracked-task counter. Called from the wrapper
    /// promise destructor through on_spawn_completion_, so the counter is
    /// balanced even if the handle is destroyed before its body runs.
    /// Notifies waiters when the counter transitions to zero.
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

    /// Try to hand an independent coroutine to the scheduler for its first
    /// execution. This detaches any construction-time virtual-stack ancestry.
    /// On failure the handle remains live.
    [[nodiscard]] bool try_spawn(std::coroutine_handle<> handle) {
        if (!handle) [[unlikely]] return false;
        if (!running_.load(std::memory_order_relaxed)) [[unlikely]] {
            return false;
        }
        // Detach from current thread's frame chain before spawning to another thread
        // to avoid use-after-free when this thread creates another coroutine.
        auto* promise = coro::get_promise_base(handle.address());
        if (promise) {
            promise->detach_from_parent();
        }
        return do_spawn(handle, false);
    }

    void spawn(std::coroutine_handle<> handle) {
        if (!handle) [[unlikely]] return;
        if (!try_spawn(handle)) {
            handle.destroy();
        }
    }

    /// Try to re-enqueue a suspended coroutine while preserving the logical
    /// parent established by its await chain. On failure the handle remains
    /// live, so the caller must either resume it or destroy it.
    [[nodiscard]] bool try_schedule(std::coroutine_handle<> handle) {
        if (!handle) [[unlikely]] return false;
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (detail::reject_next_schedule_for_test.exchange(
                false, std::memory_order_acq_rel)) {
            return false;
        }
#endif
        if (!running_.load(std::memory_order_relaxed)) [[unlikely]] {
            return false;
        }
        return do_spawn(handle, false);
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

    /// High-level API: fire-and-forget, with affinity to a specific worker.
    /// Affinity is set before first resume; steal attempts that observe the
    /// task on another queue bounce it back to the target worker instead of
    /// executing it there.
    /// For deterministic placement, worker_id must be less than num_threads().
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

    /// High-level API: spawn + join, with affinity to a specific worker.
    /// Affinity is set before first resume; I/O is bound to that worker's
    /// io_context, and steal attempts bounce the task back to the affinity
    /// worker instead of executing it elsewhere. Use when spawned task and
    /// caller must share fate w.r.t. thread-pool resizing. For deterministic
    /// placement, worker_id must be less than num_threads().
    template<typename F, typename... Args>
        requires (std::invocable<F, Args...> && detail::is_task_v<std::invoke_result_t<F, Args...>>)
    auto go_joinable_to(size_t worker_id, F&& f, Args&&... args)
        -> coro::join_handle<detail::task_value_t<std::invoke_result_t<F, Args...>>>
    {
        return do_go_<true, true>(worker_id, std::forward<F>(f), std::forward<Args>(args)...);
    }

    /// Spawn an existing coroutine toward worker_id. Exact placement requires
    /// worker_id < num_threads(); otherwise scheduling is best-effort fallback.
    /// This is an initial ownership handoff and detaches construction-time
    /// virtual-stack ancestry.
    void spawn_to(size_t worker_id, std::coroutine_handle<> handle) {
        (void)do_spawn_to_(worker_id, handle);
    }

    /// Try to re-enqueue a suspended coroutine toward worker_id while
    /// preserving the logical parent established by its await chain. On
    /// failure the handle remains live.
    [[nodiscard]] bool try_schedule_to(size_t worker_id,
                                       std::coroutine_handle<> handle) {
        return do_schedule_to_(worker_id, handle, false);
    }

    [[nodiscard]] size_t num_threads(std::memory_order order = std::memory_order_relaxed) const noexcept {
        return num_threads_.load(order);
    }

    [[nodiscard]] size_t pending_tasks() const noexcept {
        size_t n = num_threads_.load(std::memory_order_acquire);
        return worker_pending_load_(n);
    }

    /// Dynamically resize the worker thread pool.
    ///
    /// Must be called from outside scheduler worker threads. Calls from a
    /// worker thread are ignored after logging a warning, because the shrink
    /// path joins worker threads and would otherwise be able to deadlock.
    /// Calls after shutdown begins are also ignored; scheduler lifecycle is
    /// one-shot and no worker may be introduced during teardown.
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

        {
            std::unique_lock<std::mutex> shutdown_lock(shutdown_mutex_);
            shutdown_cv_.wait(shutdown_lock, [this] {
                return !resize_in_progress_ || shutdown_started_;
            });
            if (shutdown_started_) return;
            resize_in_progress_ = true;
        }

        try {
            if (count == 0) count = 1;
            if (count > MAX_THREADS) count = MAX_THREADS;

            size_t old_count = num_threads_.load(std::memory_order_relaxed);

            if (count > old_count) {
                std::lock_guard<std::mutex> lock(workers_mutex_);
                old_count = num_threads_.load(std::memory_order_relaxed);
                if (count > old_count) {
                    for (size_t i = old_count; i < count; ++i) {
                        if (workers_[i]) {
                            // Slot already populated by the ctor or a prior shrink.
                            // If the prior worker is still draining I/O, wait for its
                            // detached thread before restarting this slot in place.
                            wait_for_draining_worker_(i);
                            (void)try_start_worker_during_resize_(workers_[i].get());
                        } else {
                            auto worker = std::make_unique<worker_thread>(
                                this, i, wait_strategy_);
                            (void)try_start_worker_during_resize_(worker.get());
                            workers_[i] = std::move(worker);
                        }
                    }
                    // Publish the new size AFTER all slot writes are visible.
                    // Shutdown waits for resize_in_progress_ to clear before
                    // snapshotting this count, so every started worker is included.
                    num_threads_.store(count, std::memory_order_release);
                }
            } else if (count < old_count) {
                // Hold the worker lock across the entire shrink transition.
                // The lifecycle state remains unlocked: a draining worker may
                // request shutdown while the resize controller waits to join it.
                std::lock_guard<std::mutex> lock(workers_mutex_);
                old_count = num_threads_.load(std::memory_order_relaxed);
                if (count < old_count) {
                    const bool running = running_.load(std::memory_order_relaxed);
                    if (running) {
                        draining_workers_.reserve(
                            draining_workers_.size() + (old_count - count));
                        mark_draining_range_(count, old_count);
                    }

                    // Update count after publishing draining slots so accounting
                    // readers never lose workers leaving the visible range.
                    num_threads_.store(count, std::memory_order_release);

                    reap_draining_workers_();

                    if (running) {
                        for (size_t i = count; i < old_count; ++i) {
                            workers_[i]->enter_draining_mode();
                            std::thread t = workers_[i]->detach_thread();
                            draining_workers_.emplace_back(i, std::move(t));
                        }
                    }
                }
            }
        } catch (...) {
            auto failure = std::current_exception();
            if (finish_resize_()) {
                try {
                    shutdown_force();
                } catch (...) {
                    // Preserve the resize failure; destruction can retry teardown.
                }
            }
            std::rethrow_exception(failure);
        }

        if (finish_resize_()) {
            shutdown_force();
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

    /// Exception handler type for unhandled exceptions in detached tasks and
    /// discarded structured-combinator branches.
    using unhandled_exception_handler = std::function<void(std::exception_ptr)>;

    /// Set the per-scheduler unhandled exception handler.
    /// Called when:
    /// - A detached task (go/go_to) throws and the exception is not observed
    /// - A when_any loser throws after the winner has already resolved
    /// - A later when_all child throws after the primary failure is fixed
    ///
    /// Default behavior (no handler set): log ERROR with exception info.
    /// When handler is set: invoke handler instead of logging.
    ///
    /// Thread-safe: handler ownership is published atomically. Can be set from
    /// any thread; racing reporters keep the loaded handler alive while calling it.
    void set_unhandled_exception_handler(unhandled_exception_handler handler) {
        std::shared_ptr<const unhandled_exception_handler> new_handler;
        if (handler) {
            new_handler = std::make_shared<unhandled_exception_handler>(
                std::move(handler));
        }
        unhandled_exception_handler_.store(
            std::move(new_handler), std::memory_order_release);
    }

    /// Get the current unhandled exception handler (may be nullptr).
    /// The returned pointer is backed by a thread-local shared snapshot and
    /// remains valid until this thread calls the getter again or exits.
    [[nodiscard]] const unhandled_exception_handler* get_unhandled_exception_handler() const noexcept {
        static thread_local std::shared_ptr<const unhandled_exception_handler> snapshot;
        snapshot = unhandled_exception_handler_.load(std::memory_order_acquire);
        return snapshot.get();
    }

    /// Report an unhandled exception. If handler is set, invoke it.
    /// Otherwise log ERROR.
    void report_unhandled_exception(std::exception_ptr ex) noexcept {
        if (!ex) return;
        auto handler = unhandled_exception_handler_.load(std::memory_order_acquire);
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
                ELIO_LOG_ERROR(
                    "unhandled exception in detached task or structured combinator: {}",
                    e.what());
            } catch (...) {
                ELIO_LOG_ERROR(
                    "unhandled exception in detached task or structured combinator: <unknown>");
            }
        }
    }

private:
    [[nodiscard]] bool shutdown_started_during_resize_() const noexcept {
        std::lock_guard<std::mutex> lock(shutdown_mutex_);
        return shutdown_started_;
    }

    [[nodiscard]] bool try_start_worker_during_resize_(
        worker_thread* worker) {
        if (!worker) return false;
        std::lock_guard<std::mutex> lock(shutdown_mutex_);
        if (shutdown_started_ ||
            !running_.load(std::memory_order_relaxed)) {
            return false;
        }
        worker->start();
        return true;
    }

    [[nodiscard]] bool finish_resize_() noexcept {
        bool run_deferred_shutdown = false;
        {
            std::lock_guard<std::mutex> lock(shutdown_mutex_);
            resize_in_progress_ = false;
            run_deferred_shutdown = shutdown_deferred_by_resize_;
            shutdown_deferred_by_resize_ = false;
        }
        shutdown_cv_.notify_all();
        return run_deferred_shutdown;
    }

    void finish_shutdown_teardown_() noexcept {
        {
            std::lock_guard<std::mutex> lock(shutdown_mutex_);
            shutdown_teardown_in_progress_ = false;
        }
        shutdown_cv_.notify_all();
    }

    /// Join worker threads left behind when shutdown was initiated by one of
    /// those workers. Only non-owner threads may call this, and only after the
    /// teardown winner has released shutdown_teardown_in_progress_.
    void join_remaining_workers_() {
        // Several external shutdown callers may wake together after a
        // worker-initiated teardown. std::thread::joinable()/join() are not
        // safe to race on the same thread object, so serialize this final
        // ownership handoff independently of the teardown winner election.
        std::lock_guard<std::mutex> join_lock(shutdown_join_mutex_);

        for (auto& worker : workers_) {
            if (worker) worker->stop();
        }

        std::lock_guard<std::mutex> lock(workers_mutex_);
        for (auto& [idx, thread] : draining_workers_) {
            if (thread.joinable()) thread.join();
            if (workers_[idx]) workers_[idx]->leave_draining_mode();
            clear_draining_slot_(idx);
        }
        draining_workers_.clear();
    }

    static std::exception_ptr spawn_rejected_exception_() noexcept {
        try {
            throw std::logic_error("scheduler rejected joinable task before execution");
        } catch (...) {
            return std::current_exception();
        }
    }

    [[nodiscard]] size_t worker_pending_load_at_(size_t index) const noexcept {
        auto* worker = workers_[index].get();
        if (!worker) return 0;
        const auto& ctx = worker->io_context();
        return worker->queue_size() +
               std::max(ctx.pending_count(), ctx.active_pin_count());
    }

    [[nodiscard]] size_t worker_pending_load_(size_t visible_count) const noexcept {
        size_t total = 0;
        for (size_t i = 0; i < visible_count; ++i) {
            total += worker_pending_load_at_(i);
        }
        // Fast path: skip the draining-slot scan entirely when no slots are
        // draining (the common case after startup and when no shrink is active).
        if (draining_count_.load(std::memory_order_acquire) > 0) {
            size_t upper = draining_upper_bound_.load(std::memory_order_acquire);
            for (size_t i = visible_count; i < upper; ++i) {
                if (draining_slots_[i].load(std::memory_order_acquire)) {
                    total += worker_pending_load_at_(i);
                }
            }
        }
        return total;
    }

    /// Mark slots [first, last) as draining and update accounting counters.
    /// Must be called under workers_mutex_ before publishing the new num_threads_.
    void mark_draining_range_(size_t first, size_t last) noexcept {
        if (first >= last) return;
        for (size_t i = first; i < last; ++i) {
            draining_slots_[i].store(true, std::memory_order_release);
        }
        // Publish the scan bound before incrementing the count. Readers use
        // draining_count_ as the fast-path gate; once they observe count > 0,
        // they must also be able to observe an upper bound covering this range.
        size_t expected = draining_upper_bound_.load(std::memory_order_relaxed);
        while (expected < last) {
            if (draining_upper_bound_.compare_exchange_weak(
                    expected, last,
                    std::memory_order_release, std::memory_order_relaxed))
                break;
        }
        draining_count_.fetch_add(last - first, std::memory_order_release);
    }

    /// Clear a single draining slot and update accounting counters.
    /// Must be called under workers_mutex_.
    void clear_draining_slot_(size_t index) noexcept {
        draining_slots_[index].store(false, std::memory_order_release);
        // When the last draining slot is cleared, reset the upper bound so
        // subsequent accounting reads take the O(1) early-exit path.
        if (draining_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            draining_upper_bound_.store(0, std::memory_order_release);
        }
    }

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
        p.set_spawn_completion(+[](void* self) noexcept {
            static_cast<scheduler*>(self)->on_task_completed();
        }, this);
    }

    bool do_go_task_linked_(coro::cancel_token parent,
                            coro::task<void>&& wrapper) {
        auto wrapper_handle = coro::detail::task_access::handle(wrapper);
        wrapper_handle.promise().link_parent_cancellation(std::move(parent));

        auto handle = coro::detail::task_access::release(std::move(wrapper));
        handle.promise().detached_ = true;
        handle.promise().detach_from_parent();
        mark_tracked_(handle.promise());
        try {
            return do_spawn(handle);
        } catch (...) {
            handle.destroy();
            throw;
        }
    }

    template<bool Joinable, bool Pinned, typename F, typename... Args>
    auto do_go_(size_t worker_id, F&& f, Args&&... args) {
        using ResultTask = std::invoke_result_t<F, Args...>;
        using T = detail::task_value_t<ResultTask>;

        auto wrapper = [&]() {
            if constexpr (Joinable) {
                return detail::callable_wrapper(std::forward<F>(f), std::forward<Args>(args)...);
            } else {
                return detail::callable_wrapper_void(std::forward<F>(f), std::forward<Args>(args)...);
            }
        }();

        auto handle = coro::detail::task_access::release(std::move(wrapper));
        handle.promise().detached_ = true;
        if constexpr (Pinned) {
            handle.promise().set_affinity(worker_id);
        }
        handle.promise().detach_from_parent();

        if constexpr (Joinable) {
            auto state = std::make_shared<coro::detail::join_state<T>>(
                handle.promise().execution_context());
            handle.promise().join_state_ = state;
            mark_tracked_(handle.promise());
            bool scheduled = false;
            if constexpr (Pinned) {
                scheduled = do_spawn_to_(worker_id, handle);
            } else {
                scheduled = do_spawn(handle);
            }
            if (!scheduled) {
                state->set_exception(spawn_rejected_exception_());
            }
            return coro::join_handle<T>(std::move(state));
        } else {
            mark_tracked_(handle.promise());
            if constexpr (Pinned) {
                (void)do_spawn_to_(worker_id, handle);
            } else {
                (void)do_spawn(handle);
            }
        }
    }

    void reap_draining_workers_() noexcept {
        auto it = draining_workers_.begin();
        while (it != draining_workers_.end()) {
            auto& [idx, t] = *it;
            auto* worker = workers_[idx].get();
            if (!worker || !worker->is_running()) {
                if (t.joinable()) t.join();
                if (worker) worker->leave_draining_mode();
                clear_draining_slot_(idx);
                it = draining_workers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void wait_for_draining_worker_(size_t index) noexcept {
        auto it = std::find_if(draining_workers_.begin(), draining_workers_.end(),
            [index](const auto& entry) { return entry.first == index; });
        if (it == draining_workers_.end()) return;

        auto* worker = workers_[index].get();
        if (worker && worker->is_running()) {
#ifdef ELIO_RUNTIME_TEST_HOOKS
            detail::resize_waiting_for_draining_worker_for_test.store(
                true, std::memory_order_release);
            detail::resize_waiting_for_draining_worker_for_test.notify_all();
#endif
            while (worker->is_running()) {
                if (shutdown_started_during_resize_()) {
                    worker->request_stop();
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
#ifdef ELIO_RUNTIME_TEST_HOOKS
            detail::resize_waiting_for_draining_worker_for_test.store(
                false, std::memory_order_release);
#endif
        }
        if (it->second.joinable()) it->second.join();
        if (worker) worker->leave_draining_mode();
        clear_draining_slot_(index);
        draining_workers_.erase(it);
    }

    bool do_spawn_to_(size_t worker_id, std::coroutine_handle<> handle,
                      bool destroy_on_failure = true) {
        return do_enqueue_to_(worker_id, handle, destroy_on_failure, true);
    }

    bool do_schedule_to_(size_t worker_id, std::coroutine_handle<> handle,
                         bool destroy_on_failure = true) {
        return do_enqueue_to_(worker_id, handle, destroy_on_failure, false);
    }

    /// Route an actively I/O-pinned coroutine to the exact backend owner.
    /// The owner may be outside num_threads() while a shrink is draining it,
    /// so this deliberately addresses the fixed worker slot directly.
    [[nodiscard]] bool schedule_active_io_pin_(
        coro::promise_base* promise,
        std::coroutine_handle<> handle) {
        if (!promise || !promise->has_active_io_pin()) return false;

        const size_t owner = promise->io_owner_worker();
        if (owner >= MAX_THREADS) return false;
        auto* worker = workers_[owner].get();
        if (!worker || !worker->is_running() ||
            worker->io_context().generation() !=
                promise->io_context_generation()) {
            return false;
        }
        return worker->schedule(handle);
    }

    bool do_enqueue_to_(size_t worker_id, std::coroutine_handle<> handle,
                        bool destroy_on_failure, bool detach_parent) {
        if (!handle) [[unlikely]] return false;
        if (!running_.load(std::memory_order_relaxed)) [[unlikely]] {
            if (destroy_on_failure) handle.destroy();
            return false;
        }

        auto* promise = coro::get_promise_base(handle.address());
        if (detach_parent && promise) {
            // Initial ownership handoff must not retain construction-time
            // ancestry. Suspended-coroutine migration preserves await ancestry.
            promise->detach_from_parent();
        }

        if (promise && promise->has_active_io_pin()) {
            if (schedule_active_io_pin_(promise, handle)) {
                return true;
            }
            if (destroy_on_failure) handle.destroy();
            return false;
        }

        size_t n = num_threads_.load(std::memory_order_acquire);
        if (n == 0) [[unlikely]] {
            if (destroy_on_failure) handle.destroy();
            return false;
        }

        if (promise && promise->is_worker_local()) {
            size_t affinity = promise->effective_affinity();
            if (affinity == worker_id && affinity < MAX_THREADS &&
                workers_[affinity] && workers_[affinity]->schedule(handle)) {
                return true;
            }
            if (destroy_on_failure) handle.destroy();
            return false;
        }

        // Public pinning APIs require worker_id < num_threads() for exact
        // placement. Out-of-range values are treated as best-effort enqueue
        // fallback rather than a stable placement contract.
        size_t idx = worker_id % n;
        // Verify the chosen slot is still running. After a shrink the worker at
        // this index may have been stopped (or be in the process of being
        // stopped) — pushing into its inbox would orphan the task. Fall through
        // to do_spawn() so the round-robin path picks a worker that is still
        // accepting work.
        if (workers_[idx] && workers_[idx]->schedule(handle)) {
            return true;
        }
        return do_spawn(handle, destroy_on_failure);
    }

    bool do_spawn(std::coroutine_handle<> handle,
                  bool destroy_on_failure = true) {
        if (!handle) [[unlikely]] return false;
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (detail::reject_next_spawn_for_test.exchange(
                false, std::memory_order_acq_rel)) {
            if (destroy_on_failure) handle.destroy();
            return false;
        }
#endif
        // Release fence ensures all writes to the coroutine frame (including
        // captured lambda state) are visible to the worker that will run this task
        std::atomic_thread_fence(std::memory_order_release);

        size_t n = num_threads_.load(std::memory_order_acquire);
        if (n == 0) [[unlikely]] {
            if (destroy_on_failure) handle.destroy();
            return false;
        }

        // Check if task has affinity - if so, schedule to that specific worker.
        auto* promise = coro::get_promise_base(handle.address());
        if (promise && promise->has_active_io_pin()) {
            if (schedule_active_io_pin_(promise, handle)) {
                return true;
            }
            if (destroy_on_failure) handle.destroy();
            return false;
        }
        size_t affinity = promise ? promise->effective_affinity()
                                  : coro::NO_AFFINITY;
        if (affinity != coro::NO_AFFINITY && affinity < n) {
            if (workers_[affinity] && workers_[affinity]->schedule(handle)) {
                return true;
            }
            if (promise && promise->is_worker_local()) {
                if (destroy_on_failure) handle.destroy();
                return false;
            }
            // Target worker not running - clear affinity and fall through
            if (promise) {
                promise->clear_affinity();
            }
        } else if (affinity != coro::NO_AFFINITY && promise &&
                   promise->is_worker_local()) {
            if (affinity < MAX_THREADS && workers_[affinity] &&
                workers_[affinity]->schedule(handle)) {
                return true;
            }
            if (destroy_on_failure) handle.destroy();
            return false;
        }

        // No affinity or invalid affinity - round-robin to any running worker
        size_t start_index = spawn_index_.fetch_add(1, std::memory_order_relaxed) % n;
        for (size_t i = 0; i < n; ++i) {
            size_t index = (start_index + i) % n;
            if (workers_[index] && workers_[index]->schedule(handle)) {
                return true;
            }
        }

        // All workers stopped, try again with current thread count
        n = num_threads_.load(std::memory_order_acquire);
        if (n > 0 && workers_[0] && workers_[0]->schedule(handle)) {
            return true;
        } else {
            if (destroy_on_failure) handle.destroy();
            return false;
        }
    }

    /// Preserve progress for a borrowed continuation when its current owner
    /// worker rejected an external-queue insertion. Publishing to the owner's
    /// local deque defers execution until the current coroutine returns, so it
    /// neither recurses nor bypasses affinity or active-I/O ownership.
    [[nodiscard]] bool try_schedule_current_worker_local_(
        std::coroutine_handle<> handle) noexcept {
        auto* current = worker_thread::current();
        if (!handle || !current || current->scheduler_ != this) {
            return false;
        }

        auto* promise = coro::get_promise_base(handle.address());
        if (promise && promise->has_active_io_pin()) {
            if (!promise->is_io_pin_owner(
                    current->worker_id(),
                    current->io_context().generation())) {
                return false;
            }
        } else if (promise) {
            const size_t affinity = promise->effective_affinity();
            if (promise->is_worker_local()) {
                if (affinity != current->worker_id()) {
                    return false;
                }
            } else if (affinity != coro::NO_AFFINITY) {
                const size_t n = num_threads_.load(std::memory_order_acquire);
                if (affinity < n && affinity != current->worker_id()) {
                    return false;
                }
                if (affinity >= n) {
                    // Match do_spawn(): invalid best-effort affinity does not
                    // prevent an otherwise movable task from making progress.
                    promise->clear_affinity();
                }
            }
        }

        std::atomic_thread_fence(std::memory_order_release);
        current->schedule_local(handle);
#ifdef ELIO_RUNTIME_TEST_HOOKS
        detail::local_schedule_fallbacks_for_test.fetch_add(
            1, std::memory_order_release);
#endif
        return true;
    }

    /// Report whether retrying normal scheduler routing can still make
    /// progress. Exact worker-local and active-I/O ownership must remain live;
    /// movable tasks only need one visible worker that is still accepting work.
    [[nodiscard]] bool has_live_schedule_target_(
        std::coroutine_handle<> handle) const noexcept {
        if (!handle || !running_.load(std::memory_order_acquire)) {
            return false;
        }

        auto* promise = coro::get_promise_base(handle.address());
        if (promise && promise->has_active_io_pin()) {
            const size_t owner = promise->io_owner_worker();
            if (owner >= MAX_THREADS) return false;
            const auto* worker = workers_[owner].get();
            return worker && worker->is_running() &&
                   worker->io_context().generation() ==
                       promise->io_context_generation();
        }

        if (promise && promise->is_worker_local()) {
            const size_t owner = promise->effective_affinity();
            return owner < MAX_THREADS && workers_[owner] &&
                   workers_[owner]->is_running();
        }

        const size_t n = num_threads_.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i) {
            if (workers_[i] && workers_[i]->is_running()) {
                return true;
            }
        }
        return false;
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

    // Slots whose worker threads are still draining after a shrink. This gives
    // lock-free accounting readers a stable way to include workers that are no
    // longer inside the num_threads_ visible range.
    std::array<std::atomic<bool>, MAX_THREADS> draining_slots_{};
    // Number of currently-active draining slots. When zero, accounting readers
    // skip the draining-slot scan entirely (O(1) fast path).
    std::atomic<size_t> draining_count_{0};
    // Exclusive upper bound of draining slot indices. The scan loop in
    // worker_pending_load_() only needs to iterate up to this value. Updated
    // (max-only) when slots are marked, reset to 0 when draining_count_ reaches
    // zero.
    std::atomic<size_t> draining_upper_bound_{0};

    // Frequently-read fields on their own cache line to avoid false sharing
    // with the spawn counter and the slow-path workers_mutex_.
    alignas(64) std::atomic<size_t> num_threads_;
    std::atomic<bool> running_;
    std::atomic<bool> paused_;

    // running_=false means no new work is accepted; teardown completion is a
    // separate lifecycle phase because a worker can initiate shutdown and must
    // unwind before an external thread can join it.
    mutable std::mutex shutdown_mutex_;
    std::condition_variable shutdown_cv_;
    bool shutdown_teardown_in_progress_ = false;
    bool shutdown_started_ = false;
    bool resize_in_progress_ = false;
    bool shutdown_deferred_by_resize_ = false;
    std::mutex shutdown_join_mutex_;

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
    // active_tracked_ is incremented before spawning a scheduler-owned wrapper
    // coroutine and decremented from that wrapper promise's destructor.
    // waiters_ is a hint so the on-completion path skips the CV mutex when no
    // one is parked in wait_for_idle.
    alignas(64) std::atomic<size_t> active_tracked_{0};
    alignas(64) std::atomic<size_t> waiters_{0};
    mutable std::mutex idle_mutex_;
    mutable std::condition_variable idle_cv_;

    // Join handles for workers that are draining pending I/O after a shrink
    // operation. The worker objects stay in workers_ slots so stale lock-free
    // readers that loaded the old thread count never race with unique_ptr moves.
    // Protected by workers_mutex_.
    std::vector<std::pair<size_t, std::thread>> draining_workers_;

    // Per-scheduler unhandled exception handler. Empty means default behavior
    // (log ERROR). Atomic shared ownership keeps a loaded handler alive while a
    // racing set_unhandled_exception_handler() publishes a replacement.
    std::atomic<std::shared_ptr<const unhandled_exception_handler>> unhandled_exception_handler_{};

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
    if (sched) {
        // Rejection leaves this borrowed handle live. Prefer normal routing;
        // when the current worker is the valid owner, defer through its local
        // deque so a full inbox cannot make that worker spin instead of drain.
        while (true) {
            if (sched->is_running() && sched->try_schedule(handle)) {
                return;
            }
            if (sched->try_schedule_current_worker_local_(handle)) {
                return;
            }
            if (!sched->is_running() ||
                !sched->has_live_schedule_target_(handle)) {
                // schedule_handle() borrows the frame and cannot destroy or
                // adopt it without coordinating with the owning task object.
                // No thread is safe for an exact-owner continuation here;
                // leave lifetime resolution to that owner/shutdown contract.
                return;
            }
            std::this_thread::yield();
        }
    }

    static thread_local std::vector<std::coroutine_handle<>> trampoline_queue;
    static thread_local bool trampoline_running = false;

    trampoline_queue.push_back(handle);

    if (!trampoline_running) {
        trampoline_running = true;
        struct trampoline_guard {
            std::vector<std::coroutine_handle<>>& q;
            ~trampoline_guard() {
                // Only destroy completed coroutines.  Live (suspended)
                // handles are intentionally leaked: there is no scheduler
                // to adopt them, and destroying them here would UAF any
                // awaitables holding raw coroutine_handle references.
                size_t leaked = 0;
                for (auto h : q) {
                    if (h.done()) {
                        h.destroy();
                    } else {
                        ++leaked;
                    }
                }
                if (leaked > 0) {
                    ELIO_LOG_WARNING("trampoline: {} live coroutine(s) leaked (no scheduler available)", leaked);
                }
                q.clear();
                trampoline_running = false;
            }
        } guard{trampoline_queue};
        while (!trampoline_queue.empty()) {
            auto h = trampoline_queue.back();
            trampoline_queue.pop_back();
            if (!h.done()) {
                auto* promise = coro::get_promise_base(h.address());
                coro::detail::frame_context_scope frame_scope(promise);
                h.resume();
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
    std::lock_guard<std::mutex> lock(schedule_mutex_);
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    thread_ = std::thread(&worker_thread::run, this);
}

inline void worker_thread::request_stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(schedule_mutex_);
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false,
                std::memory_order_release, std::memory_order_relaxed)) return;
    }
    wake();  // Wake the worker if it's blocked in I/O poll
}

inline void worker_thread::stop() {
    request_stop();
    if (!thread_.joinable()) return;
    if (thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
}

/// Final cleanup for any orphaned tasks - only call after ALL workers have stopped.
/// This is a safety net for edge cases where tasks might still exist after drain phase.
inline void worker_thread::drain_remaining_tasks() noexcept {
    // First drain external queues to the local deque.
    drain_inbox();
    void* addr;
    // Destroy any remaining tasks (should be rare after drain phase in run())
    while ((addr = queue_->pop()) != nullptr) {
        auto handle = std::coroutine_handle<>::from_address(addr);
        if (handle) {
            handle.destroy();
        }
    }
}

/// Run worker-local maintenance tasks on this retiring worker; redistribute
/// normal user tasks to active workers so they cannot start fresh I/O on a
/// worker whose io_context is about to stop being polled.
inline bool worker_thread::run_or_redistribute_retiring_task(
    scheduler* sched,
    std::coroutine_handle<> handle) noexcept {
    auto* promise = coro::get_promise_base(handle.address());
    if (promise && promise->has_active_io_pin()) {
        if (promise->is_io_pin_owner(
                worker_id_, io_context_->generation())) {
            needs_sync_ = true;
            run_task(handle);
            return true;
        }

        // An active operation may only resume on the backend owner. Never
        // clear this pin or execute the continuation on the retiring worker.
        return sched->try_schedule(handle);
    }

    if (promise && promise->is_worker_local() &&
        promise->effective_affinity() == worker_id_) {
        needs_sync_ = true;
        run_task(handle);
        return true;
    }

    if (sched->try_schedule(handle)) {
        return true;
    }

    // A concurrent full shutdown may start after this worker entered its
    // retirement drain. In that case there is no surviving worker to accept
    // the handle, so finish it on this worker instead of retrying forever.
    if (!sched->is_running()) {
        needs_sync_ = true;
        run_task(handle);
        return true;
    }

    return false;
}

/// Redistribute remaining tasks to active workers - call during thread pool shrink
inline void worker_thread::redistribute_tasks(scheduler* sched) noexcept {
    // First drain external queues to the local deque.
    drain_inbox();
    void* addr;
    // Then redistribute all tasks to active workers
    while ((addr = queue_->pop()) != nullptr) {
        auto handle = std::coroutine_handle<>::from_address(addr);
        if (handle && !handle.done()) {
            if (!run_or_redistribute_retiring_task(sched, handle)) {
                queue_->push(handle.address());
                return;
            }
        } else if (handle) {
            handle.destroy();
        }
    }
}

inline void worker_thread::drain_inbox() noexcept {
    // A producer that arrives after this observation will wake the worker and
    // be drained on the next pass. Avoid transfer-lock traffic on the normal
    // local execution path when there is no cross-thread work to transfer.
    if (inbox_->empty() &&
        overflow_size_.load(std::memory_order_acquire) == 0) {
        return;
    }
#ifdef ELIO_RUNTIME_TEST_HOOKS
    if (detail::queue_snapshot_paused_for_test.load(
            std::memory_order_acquire)) {
        detail::queue_transfer_waiting_for_test.store(
            true, std::memory_order_release);
        detail::queue_transfer_waiting_for_test.notify_all();
    }
#endif
    std::lock_guard<std::mutex> transfer_lock(transfer_mutex_);
#ifdef ELIO_RUNTIME_TEST_HOOKS
    detail::queue_transfer_waiting_for_test.store(false,
                                                  std::memory_order_release);
#endif

    // Drain the MPSC fast path into the local Chase-Lev deque.
    void* item;
    while ((item = inbox_->pop()) != nullptr) {
        queue_->push(item);
    }

    // Overflow is used only after bounded inbox retries. Swap under the lock
    // so producers are never held while the worker updates its local deque.
    std::deque<void*> overflow;
    {
        std::lock_guard<std::mutex> lock(overflow_mutex_);
        overflow.swap(overflow_);
    }

    const size_t transfer_size = overflow.size();
#ifdef ELIO_RUNTIME_TEST_HOOKS
    if (transfer_size > 0 &&
        detail::pause_overflow_transfer_for_test.load(std::memory_order_acquire)) {
        detail::overflow_transfer_paused_for_test.store(true,
                                                        std::memory_order_release);
        detail::overflow_transfer_paused_for_test.notify_all();
        while (detail::pause_overflow_transfer_for_test.load(
            std::memory_order_acquire)) {
            detail::pause_overflow_transfer_for_test.wait(true,
                                                          std::memory_order_acquire);
        }
        detail::overflow_transfer_paused_for_test.store(false,
                                                        std::memory_order_release);
    }
#endif
    while (!overflow.empty()) {
        queue_->push(overflow.front());
        overflow.pop_front();
    }
    if (transfer_size > 0) {
        // Publish every handle to queue_ before removing the transfer batch
        // from accounting. Readers may briefly overcount, but never observe a
        // false idle window between the overflow queue and local deque.
        overflow_size_.fetch_sub(transfer_size, std::memory_order_release);
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
    io_context_->bind_owner_thread();

    while (running_.load(std::memory_order_relaxed)) {
        if (draining_.load(std::memory_order_acquire)) [[unlikely]] {
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (detail::hold_draining_worker_for_test.load(
                    std::memory_order_acquire)) {
                detail::draining_worker_held_for_test.store(
                    true, std::memory_order_release);
                detail::draining_worker_held_for_test.notify_all();
                io_context_->poll(std::chrono::milliseconds(1));
                continue;
            }
            detail::draining_worker_held_for_test.store(
                false, std::memory_order_release);
#endif
            // Draining mode: only poll I/O, no new tasks or stealing.
            // Redistribute any queued tasks, then wait for pending I/O to complete.
            redistribute_tasks(scheduler_);
            if (io_context_->pending_count() == 0 &&
                io_context_->active_pin_count() == 0) {
                {
                    std::lock_guard<std::mutex> lock(schedule_mutex_);
                    running_.store(false, std::memory_order_release);
                }
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

#ifdef ELIO_RUNTIME_TEST_HOOKS
    detail::draining_worker_held_for_test.store(
        false, std::memory_order_release);
#endif

    // Drain phase: after running_ becomes false, continue executing all
    // remaining tasks until both local queue and inbox are empty.
    // This ensures shutdown() returns only when all submitted tasks have
    // fully completed (including coroutine cleanup and lambda destruction).
    //
    // Retirement vs. shutdown: if the SCHEDULER is still running, this
    // worker is being retired by a pool shrink (set_thread_count), not a
    // full shutdown. In that case queued tasks must be redistributed to
    // active workers rather than resumed here — resuming a queued coroutine
    // on the retiring worker lets it start a fresh async I/O that binds to
    // this worker's io_context (via worker_thread::current()), which stops
    // being polled once this thread exits, orphaning the operation and
    // hanging the coroutine. shutdown_force() flips the scheduler's
    // running_ to false BEFORE joining workers, so a genuine shutdown still
    // takes the run_task() path below and drains locally.
    //
    // Note: pop() (steal-style CAS path) — NOT pop_local(false). Other
    // workers may still be inside try_steal()->steal_task() against this
    // worker's deque because they haven't yet observed our running_=false.
    // Chase-Lev requires either single-threaded owner pop OR concurrent
    // steal+pop synchronized via the seq_cst fence inside pop(); the
    // single-thread fast path of pop_local() races with those stealers.
    const bool retiring = scheduler_->is_running();
    while (true) {
        drain_inbox();
        void* addr = queue_->pop();
        if (!addr) break;

        auto handle = std::coroutine_handle<>::from_address(addr);
        if (handle && !handle.done()) {
            if (retiring) {
                // Hand normal tasks back to the scheduler; do_spawn() routes
                // them to a surviving worker (num_threads_ was already lowered
                // before this worker was stopped, so they never land back here).
                // Worker-local maintenance tasks are the exception: they touch
                // this worker's io_context and must run here before exit.
                if (!run_or_redistribute_retiring_task(scheduler_, handle)) {
                    queue_->push(handle.address());
                    std::this_thread::yield();
                }
            } else {
                needs_sync_ = true;  // Conservatively ensure memory visibility for drained tasks
                run_task(handle);
            }
        } else if (handle) {
            handle.destroy();
        }
    }
    
    // Clear the references when done
    io_context_->unbind_owner_thread();
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

    // Restore the virtual-stack frame context while user code is running.
    auto* promise = coro::get_promise_base(handle.address());
    coro::detail::frame_context_scope frame_scope(promise);
    handle.resume();
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
            auto* promise = coro::get_promise_base(handle.address());
            size_t affinity = promise ? promise->effective_affinity()
                                      : coro::NO_AFFINITY;

            if (promise && promise->has_active_io_pin()) {
                if (promise->is_io_pin_owner(
                        worker_id_, io_context_->generation())) {
                    steals_executed_.fetch_add(1, std::memory_order_relaxed);
                    return handle;
                }

                if (!scheduler_->schedule_active_io_pin_(promise, handle)) {
                    // Preserve the borrowed suspended frame while its backend
                    // owner remains unavailable. Returning it to the victim is
                    // preferable to executing it against the wrong context.
                    if (!victim->schedule(handle)) {
                        handle.destroy();
                    }
                }
                if (++retry_count >= max_retries) {
                    return nullptr;
                }
                i = static_cast<size_t>(-1);
                start = (steal_start + retry_count) % num_workers;
                continue;
            }

            if (promise && promise->is_worker_local()) {
                if (affinity == worker_id_) {
                    steals_executed_.fetch_add(1, std::memory_order_relaxed);
                    return handle;
                }
                if (affinity < num_workers) {
                    if (!scheduler_->try_schedule_to(affinity, handle)) {
                        // Worker-local maintenance tasks are detached runtime
                        // work and cannot execute away from their owner.
                        handle.destroy();
                    }
                } else {
                    handle.destroy();
                }
                if (++retry_count >= max_retries) {
                    return nullptr;
                }
                i = static_cast<size_t>(-1);
                start = (steal_start + retry_count) % num_workers;
                continue;
            }
            
            if (affinity == coro::NO_AFFINITY || affinity == worker_id_) {
                steals_executed_.fetch_add(1, std::memory_order_relaxed);
                return handle;
            }
            
            // Task has affinity for another worker - schedule it there
            if (affinity < num_workers) {
                if (!scheduler_->try_schedule_to(affinity, handle)) {
                    // This is a borrowed suspended handle. Keep it live and
                    // execute locally rather than consuming its owner's frame.
                    if (promise) {
                        promise->clear_affinity();
                    }
                    steals_executed_.fetch_add(1, std::memory_order_relaxed);
                    return handle;
                }
            } else {
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

    // Mark as idle before any blocking so diagnostics can observe wait state.
    idle_.store(true, std::memory_order_release);

    // Optional spinning phase (if configured via wait_strategy)
    if (strategy_.spin_iterations > 0) {
        for (size_t i = 0; i < strategy_.spin_iterations; ++i) {
            if (inbox_->size_approx() > 0 ||
                overflow_size_.load(std::memory_order_acquire) > 0) {
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
