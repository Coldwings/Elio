#pragma once

#include "worker_thread.hpp"
#include "blocking_pool.hpp"
#include <elio/log/macros.hpp>
#include <elio/coro/frame.hpp>
#include <elio/coro/vthread_stack.hpp>
#include <elio/coro/task.hpp>
#include <type_traits>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <coroutine>
#include <thread>
#include <functional>

namespace elio::runtime {

namespace detail {
    // Type traits for task<T>
    template<typename T> struct task_value;
    template<typename T> struct task_value<coro::task<T>> { using type = T; };
    template<typename T> using task_value_t = typename task_value<T>::type;

    template<typename T> struct is_task : std::false_type {};
    template<typename T> struct is_task<coro::task<T>> : std::true_type {};
    template<typename T> inline constexpr bool is_task_v = is_task<T>::value;

    /// Wrapper coroutine for fire-and-forget (go/go_to).
    /// Stores callable and args in its coroutine frame, ensuring lambda lifetime
    /// safety even when temporary lambdas are passed.
    ///
    /// The inner coroutine's frame holds a reference to the lambda (via this pointer),
    /// which remains valid because this wrapper's frame outlives the inner coroutine.
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
        // Pre-reserve to MAX_THREADS to prevent reallocation during runtime
        // This ensures get_worker() is safe while set_thread_count() adds workers
        workers_.reserve(MAX_THREADS);
        for (size_t i = 0; i < n; ++i) {
            workers_.push_back(std::make_unique<worker_thread>(this, i, strategy));
        }
    }

    ~scheduler() {
        if (running_.load(std::memory_order_relaxed)) {
            shutdown();
        }
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

    void shutdown() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;
        }
        
        // First shutdown blocking pool (before stopping workers)
        if (blocking_pool_) {
            blocking_pool_->shutdown();
        }
        
        // Then stop all workers (sets running_=false and joins threads)
        for (auto& worker : workers_) {
            worker->stop();
        }
        
        // Now that ALL workers are stopped, drain remaining tasks
        // This is safe because no worker can steal from another at this point
        for (auto& worker : workers_) {
            worker->drain_remaining_tasks();
        }
        
        if (current_scheduler_ == this) {
            current_scheduler_ = nullptr;
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
    /// @param f  Callable that returns a task<T>
    /// @param args  Arguments to forward to the callable
    ///
    /// Note: f and args are safely stored in a wrapper coroutine's frame,
    /// so temporary lambdas with captures are safe to use.
    template<typename F, typename... Args>
        requires (std::invocable<F, Args...> && detail::is_task_v<std::invoke_result_t<F, Args...>>)
    void go(F&& f, Args&&... args) {
        // Create independent vthread_stack for spawned coroutine
        auto* new_vstack = new coro::vthread_stack();
        
        // Save current vstack context and switch to new one
        auto* old_vstack = coro::vthread_stack::current();
        coro::vthread_stack::set_current(new_vstack);
        
        // Create wrapper coroutine - f and args are stored in wrapper's frame
        // This ensures lambda captures remain valid for the entire coroutine lifetime
        auto wrapper = detail::callable_wrapper_void(std::forward<F>(f), std::forward<Args>(args)...);
        
        // Restore previous vstack context
        coro::vthread_stack::set_current(old_vstack);
        
        auto handle = coro::detail::task_access::release(wrapper);
        handle.promise().detached_ = true;
        handle.promise().set_vstack_owner(new_vstack);
        // Detach from current thread's frame chain before spawning to another thread
        // to avoid use-after-free when this thread creates another coroutine.
        handle.promise().detach_from_parent();
        do_spawn(handle);
    }

    /// High-level API: fire-and-forget, spawn to specific worker
    /// @param worker_id  Target worker index
    /// @param f  Callable that returns a task<T>
    /// @param args  Arguments to forward to the callable
    ///
    /// Note: f and args are safely stored in a wrapper coroutine's frame,
    /// so temporary lambdas with captures are safe to use.
    template<typename F, typename... Args>
        requires (std::invocable<F, Args...> && detail::is_task_v<std::invoke_result_t<F, Args...>>)
    void go_to(size_t worker_id, F&& f, Args&&... args) {
        // Create independent vthread_stack for spawned coroutine
        auto* new_vstack = new coro::vthread_stack();
        
        // Save current vstack context and switch to new one
        auto* old_vstack = coro::vthread_stack::current();
        coro::vthread_stack::set_current(new_vstack);
        
        // Create wrapper coroutine - f and args are stored in wrapper's frame
        // This ensures lambda captures remain valid for the entire coroutine lifetime
        auto wrapper = detail::callable_wrapper_void(std::forward<F>(f), std::forward<Args>(args)...);
        
        // Restore previous vstack context
        coro::vthread_stack::set_current(old_vstack);
        
        auto handle = coro::detail::task_access::release(wrapper);
        handle.promise().detached_ = true;
        handle.promise().set_vstack_owner(new_vstack);
        // Detach from current thread's frame chain before spawning to another thread
        // to avoid use-after-free when this thread creates another coroutine.
        handle.promise().detach_from_parent();
        spawn_to(worker_id, handle);
    }

    /// High-level API: spawn + join, spawn to this scheduler
    /// @param f  Callable that returns a task<T>
    /// @param args  Arguments to forward to the callable
    /// @return join_handle<T> that can be awaited to get the result
    ///
    /// Note: f and args are safely stored in a wrapper coroutine's frame,
    /// so temporary lambdas with captures are safe to use.
    template<typename F, typename... Args>
        requires (std::invocable<F, Args...> && detail::is_task_v<std::invoke_result_t<F, Args...>>)
    auto go_joinable(F&& f, Args&&... args)
        -> coro::join_handle<detail::task_value_t<std::invoke_result_t<F, Args...>>>
    {
        using T = detail::task_value_t<std::invoke_result_t<F, Args...>>;
        
        // Create independent vthread_stack for spawned coroutine
        auto* new_vstack = new coro::vthread_stack();
        
        // Save current vstack context and switch to new one
        auto* old_vstack = coro::vthread_stack::current();
        coro::vthread_stack::set_current(new_vstack);
        
        // Create wrapper coroutine - f and args are stored in wrapper's frame
        // This ensures lambda captures remain valid for the entire coroutine lifetime
        auto wrapper = detail::callable_wrapper(std::forward<F>(f), std::forward<Args>(args)...);
        
        // Restore previous vstack context
        coro::vthread_stack::set_current(old_vstack);
        
        auto handle = coro::detail::task_access::release(wrapper);
        auto state = std::make_shared<coro::detail::join_state<T>>();
        handle.promise().join_state_ = state;
        handle.promise().detached_ = true;  // Enable destruction notification
        handle.promise().set_vstack_owner(new_vstack);
        // Detach from current thread's frame chain before spawning to another thread
        // to avoid use-after-free when this thread creates another coroutine.
        handle.promise().detach_from_parent();
        do_spawn(handle);
        return coro::join_handle<T>(std::move(state));
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
        workers_[worker_id % n]->schedule(handle);
    }

    [[nodiscard]] size_t num_threads() const noexcept {
        return num_threads_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t pending_tasks() const noexcept {
        size_t n = num_threads_.load(std::memory_order_acquire);
        size_t total = 0;
        for (size_t i = 0; i < n && i < workers_.size(); ++i) {
            total += workers_[i]->queue_size();
        }
        return total;
    }

    void set_thread_count(size_t count) {
        if (count == 0) count = 1;
        if (count > MAX_THREADS) count = MAX_THREADS;
        
        size_t old_count = num_threads_.load(std::memory_order_relaxed);
        
        if (count > old_count) {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            old_count = num_threads_.load(std::memory_order_relaxed);
            if (count <= old_count) return;
            
            for (size_t i = old_count; i < count; ++i) {
                if (i < workers_.size()) {
                    if (running_.load(std::memory_order_relaxed)) {
                        workers_[i]->start();
                    }
                } else {
                    auto worker = std::make_unique<worker_thread>(this, i, wait_strategy_);
                    if (running_.load(std::memory_order_relaxed)) {
                        worker->start();
                    }
                    workers_.push_back(std::move(worker));
                }
            }
            num_threads_.store(count, std::memory_order_release);
        } else if (count < old_count) {
            // Lock to prevent spawns to workers being stopped
            std::unique_lock<std::mutex> lock(workers_mutex_);
            old_count = num_threads_.load(std::memory_order_relaxed);
            if (count >= old_count) return;
            
            // Update count first so new spawns go to remaining workers
            num_threads_.store(count, std::memory_order_release);
            
            // Unlock before stopping workers (stop() joins which can be slow)
            lock.unlock();
            
            // Stop the workers that are being removed
            for (size_t i = count; i < old_count; ++i) {
                workers_[i]->stop();
            }
            
            // Redistribute remaining tasks to active workers
            for (size_t i = count; i < old_count; ++i) {
                workers_[i]->redistribute_tasks(this);
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
        for (size_t i = 0; i < n && i < workers_.size(); ++i) {
            total += workers_[i]->tasks_executed();
        }
        return total;
    }

    [[nodiscard]] size_t worker_tasks_executed(size_t worker_id) const noexcept {
        size_t n = num_threads_.load(std::memory_order_acquire);
        if (worker_id >= n || worker_id >= workers_.size()) return 0;
        return workers_[worker_id]->tasks_executed();
    }

    /// Get the wait strategy used by this scheduler
    [[nodiscard]] const wait_strategy& get_wait_strategy() const noexcept {
        return wait_strategy_;
    }

    /// Get the blocking pool for spawn_blocking operations
    [[nodiscard]] blocking_pool* get_blocking_pool() noexcept {
        return blocking_pool_.get();
    }

private:
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

    std::vector<std::unique_ptr<worker_thread>> workers_;

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
        // No scheduler - run synchronously. Task self-destructs via final_suspend.
        if (!handle.done()) handle.resume();
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
    // Set the current scheduler and worker for this thread
    scheduler::current_scheduler_ = scheduler_;
    current_worker_ = this;
    
    while (running_.load(std::memory_order_relaxed)) {
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
    while (true) {
        drain_inbox();
        void* addr = queue_->pop_local(false);  // No concurrent stealers, workers are stopping
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
    // Check single-worker mode dynamically (thread count can change at runtime)
    // Pass allow_concurrent_steals=true to pop_local() so it can fall back to
    // pop() if there might be concurrent stealers. This avoids the TOCTOU race
    // where another thread could be added between check and use.
    bool single_worker = (scheduler_->num_threads() == 1);

    // Fast path: pop from local deque first
    // In single-worker mode, use pop_local() to skip seq_cst fence
    // Pass !single_worker as allow_concurrent_steals - pop_local() will fall back
    // to pop() if there could be concurrent stealers
    void* addr = queue_->pop_local(!single_worker);
    if (addr) {
        needs_sync_ = false;  // Local task, no sync needed
        return std::coroutine_handle<>::from_address(addr);
    }

    // Local queue empty - drain any externally submitted tasks from inbox
    drain_inbox();

    // Try local deque again after draining inbox
    addr = queue_->pop_local(!single_worker);
    if (addr) {
        needs_sync_ = true;  // Came from inbox, needs sync
        return std::coroutine_handle<>::from_address(addr);
    }
    
    // Nothing local, try stealing from other workers (skip in single-worker mode)
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
                // No affinity or affinity matches this worker - we can run it
                return handle;
            }
            
            // Task has affinity for another worker - schedule it there
            if (affinity < num_workers) {
                scheduler_->spawn_to(affinity, handle);
            } else {
                // Invalid affinity (worker doesn't exist) - clear and run locally
                auto* promise = coro::get_promise_base(handle.address());
                if (promise) {
                    promise->clear_affinity();
                }
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
