#pragma once

#include "worker_thread.hpp"
#include <elio/log/macros.hpp>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <coroutine>
#include <thread>
#include <array>

namespace elio::io {
class io_context;
}

namespace elio::runtime {

/// Work-stealing scheduler for coroutines
class scheduler {
    friend class worker_thread;  // Allow workers to set current_scheduler_
    
public:
    static constexpr size_t MAX_THREADS = 256;
    
    explicit scheduler(size_t num_threads = std::thread::hardware_concurrency())
        : num_threads_(num_threads == 0 ? 1 : num_threads)
        , running_(false)
        , paused_(false)
        , spawn_index_(0) {
        
        size_t n = num_threads_.load(std::memory_order_relaxed);
        // Pre-reserve to MAX_THREADS to prevent reallocation during runtime
        // This ensures get_worker() is safe while set_thread_count() adds workers
        workers_.reserve(MAX_THREADS);
        for (size_t i = 0; i < n; ++i) {
            workers_.push_back(std::make_unique<worker_thread>(this, i));
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
        
        // First stop all workers (sets running_=false and joins threads)
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
        
        // Release fence ensures all writes to the coroutine frame (including
        // captured lambda state) are visible to the worker that will run this task
        std::atomic_thread_fence(std::memory_order_release);
        
        size_t n = num_threads_.load(std::memory_order_acquire);
        if (n == 0) [[unlikely]] {
            handle.destroy();
            return;
        }
        
        // Try to find a running worker (handles race with set_thread_count shrink)
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

    void spawn_to(size_t worker_id, std::coroutine_handle<> handle) {
        if (!handle) [[unlikely]] return;
        if (!running_.load(std::memory_order_relaxed)) [[unlikely]] {
            handle.destroy();
            return;
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
                    auto worker = std::make_unique<worker_thread>(this, i);
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

    void set_io_context(io::io_context* ctx) noexcept {
        io_context_ = ctx;
    }

    [[nodiscard]] io::io_context* get_io_context() const noexcept {
        return io_context_;
    }

    bool try_poll_io(std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

private:
    std::vector<std::unique_ptr<worker_thread>> workers_;
    std::atomic<size_t> num_threads_;
    std::atomic<bool> running_;
    std::atomic<bool> paused_;
    std::atomic<size_t> spawn_index_;
    mutable std::mutex workers_mutex_;
    io::io_context* io_context_ = nullptr;
    mutable std::mutex io_poll_mutex_;
    
    static inline thread_local scheduler* current_scheduler_ = nullptr;
};

} // namespace elio::runtime

#include <elio/io/io_context.hpp>

namespace elio::runtime {

inline bool scheduler::try_poll_io(std::chrono::milliseconds timeout) {
    if (!io_context_) return false;
    
    std::unique_lock<std::mutex> lock(io_poll_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    
    io_context_->poll(timeout);
    return true;
}

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
    if (thread_.joinable()) thread_.join();
}

/// Drain and destroy remaining tasks - only call after ALL workers have stopped
inline void worker_thread::drain_remaining_tasks() noexcept {
    // First drain inbox to deque
    void* addr;
    while ((addr = inbox_.pop()) != nullptr) {
        queue_.push(addr);
    }
    // Then destroy all tasks in the deque
    while ((addr = queue_.pop()) != nullptr) {
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
    while ((addr = inbox_.pop()) != nullptr) {
        queue_.push(addr);
    }
    // Then redistribute all tasks to active workers
    while ((addr = queue_.pop()) != nullptr) {
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
    while ((item = inbox_.pop()) != nullptr) {
        queue_.push(item);
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
    
    // Clear the references when done
    scheduler::current_scheduler_ = nullptr;
    current_worker_ = nullptr;
    
    // Note: Cleanup of remaining tasks is handled in stop() AFTER join
    // to avoid race conditions with work stealing
}

inline std::coroutine_handle<> worker_thread::get_next_task() noexcept {
    // Fast path: pop from local deque first (no synchronization needed)
    void* addr = queue_.pop();
    if (addr) {
        needs_sync_ = false;  // Local task, no sync needed
        return std::coroutine_handle<>::from_address(addr);
    }
    
    // Local queue empty - drain any externally submitted tasks from inbox
    drain_inbox();
    
    // Try local deque again after draining inbox
    addr = queue_.pop();
    if (addr) {
        needs_sync_ = true;  // Came from inbox, needs sync
        return std::coroutine_handle<>::from_address(addr);
    }
    
    // Nothing local, try stealing from other workers
    auto handle = try_steal();
    if (handle) {
        needs_sync_ = true;  // Stolen task, needs sync
    }
    return handle;
}

inline void worker_thread::run_task(std::coroutine_handle<> handle) noexcept {
    // Acquire fence only for tasks from external sources (inbox/steal)
    // Local tasks don't need synchronization - same thread visibility
    if (needs_sync_) {
        std::atomic_thread_fence(std::memory_order_acquire);
    }
    
    if (!handle || handle.done()) [[unlikely]] return;
    
    handle.resume();
    tasks_executed_.fetch_add(1, std::memory_order_relaxed);
    
    // Note: We do NOT check done() or call destroy() here.
    // If the task completed, its final_suspend will self-destruct (fire-and-forget)
    // or resume a continuation. If it suspended mid-execution (e.g., yield),
    // another thread may already be running it - touching the handle would race.
}

inline std::coroutine_handle<> worker_thread::try_steal() noexcept {
    size_t num_workers = scheduler_->num_threads();
    if (num_workers <= 1) return nullptr;
    
    // Start stealing from a different worker each time (better distribution)
    static thread_local size_t steal_start = 0;
    size_t start = steal_start;
    steal_start = (steal_start + 1) % num_workers;
    
    for (size_t i = 0; i < num_workers; ++i) {
        size_t victim_id = (start + i) % num_workers;
        if (victim_id == worker_id_) continue;
        
        auto* victim = scheduler_->get_worker(victim_id);
        if (!victim || !victim->is_running()) continue;
        
        // Try single steal - batch stealing has race conditions with owner's pop
        auto handle = victim->steal_task();
        if (handle) {
            return handle;
        }
    }
    
    return nullptr;
}

inline void worker_thread::poll_io_when_idle() {
    // Try to poll IO, otherwise just yield to let other threads run
    if (scheduler_->try_poll_io(std::chrono::milliseconds(0))) return;
    std::this_thread::yield();
}

} // namespace elio::runtime
