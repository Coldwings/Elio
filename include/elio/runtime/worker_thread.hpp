#pragma once

#include "chase_lev_deque.hpp"
#include "mpsc_queue.hpp"
#include "wait_strategy.hpp"
#include <elio/coro/promise_base.hpp>
#include <elio/io/io_context.hpp>
#include <coroutine>
#include <thread>
#include <atomic>
#include <mutex>
#include <random>
#include <chrono>

namespace elio::runtime {

class scheduler;

/// Worker thread that executes tasks from a local queue
/// and steals from other workers when idle
/// 
/// Uses a two-queue architecture for optimal scalability:
/// - MPSC inbox: lock-free queue for external task submissions
/// - Chase-Lev deque: lock-free SPMC for owner ops and work stealing
/// 
/// This design eliminates contention between external producers and
/// the owner thread, enabling linear scaling with thread count.
class worker_thread {
public:
    worker_thread(scheduler* sched, size_t worker_id,
                   wait_strategy strategy = wait_strategy::blocking())
        : scheduler_(sched)
        , worker_id_(worker_id)
        , queue_()
        , inbox_()
        , running_(false)
        , tasks_executed_(0)
        , strategy_(strategy)
        , io_context_(std::make_unique<io::io_context>()) {
    }

    ~worker_thread() {
        stop();
    }

    worker_thread(const worker_thread&) = delete;
    worker_thread& operator=(const worker_thread&) = delete;
    worker_thread(worker_thread&&) = delete;
    worker_thread& operator=(worker_thread&&) = delete;

    void start();
    void stop();
    
    /// Drain and destroy remaining tasks - only call after ALL workers have stopped
    void drain_remaining_tasks() noexcept;
    
    /// Redistribute remaining tasks to active workers - call during thread pool shrink
    void redistribute_tasks(scheduler* sched) noexcept;

    /// Schedule a task from external thread - pushes to lock-free MPSC inbox
    /// Retries with back-off if inbox is temporarily full
    void schedule(std::coroutine_handle<> handle) {
        if (!handle) [[unlikely]] return;

        // Try fast path: push to lock-free inbox
        if (inbox_.push(handle.address())) [[likely]] {
            // Lazy wake: only wake if worker is idle (waiting for work)
            // This avoids unnecessary wake syscalls when worker is busy
            // Use relaxed load - occasional extra wake is fine, we optimize for the common case
            if (idle_.load(std::memory_order_relaxed)) {
                wake();
            }
            return;
        }

        // Slow path: inbox full, retry with exponential back-off
        // Keep retrying - inbox will eventually have space as worker drains it
        int backoff = 1;
        while (true) {
            for (int i = 0; i < backoff; ++i) {
                #if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
                #else
                std::this_thread::yield();
                #endif
            }
            if (inbox_.push(handle.address())) {
                // Lazy wake: only wake if worker is idle
                if (idle_.load(std::memory_order_relaxed)) {
                    wake();
                }
                return;
            }
            backoff = std::min(backoff * 2, 1024);
            std::this_thread::yield();
        }
    }
    
    /// Schedule a task from owner thread - pushes directly to local deque
    void schedule_local(std::coroutine_handle<> handle) {
        if (!handle) [[unlikely]] return;
        queue_.push(handle.address());
    }

    [[nodiscard]] size_t tasks_executed() const noexcept {
        return tasks_executed_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t queue_size() const noexcept {
        return queue_.size() + inbox_.size_approx();
    }

    [[nodiscard]] std::coroutine_handle<> steal_task() noexcept {
        // Don't allow stealing from stopped workers
        if (!running_.load(std::memory_order_acquire)) return nullptr;
        void* addr = queue_.steal();
        return addr ? std::coroutine_handle<>::from_address(addr) : nullptr;
    }

    /// Steal multiple tasks at once for better throughput
    template<size_t N>
    size_t steal_batch(std::array<void*, N>& output) noexcept {
        return queue_.steal_batch(output);
    }

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }
    
    /// Get the worker ID for this worker thread
    [[nodiscard]] size_t worker_id() const noexcept {
        return worker_id_;
    }
    
    /// Get the io_context for this worker thread
    [[nodiscard]] io::io_context& io_context() noexcept {
        return *io_context_;
    }
    
    /// Get the current worker thread (if called from a worker thread)
    [[nodiscard]] static worker_thread* current() noexcept {
        return current_worker_;
    }
    
    /// Wake this worker if it's sleeping (called from other threads)
    void wake() noexcept {
        io_context_->notify();
    }

    /// Get the wait strategy for this worker
    [[nodiscard]] const wait_strategy& get_wait_strategy() const noexcept {
        return strategy_;
    }

    /// Set the wait strategy for this worker
    void set_wait_strategy(wait_strategy strategy) noexcept {
        strategy_ = strategy;
    }

private:
    void run();
    void drain_inbox() noexcept;
    [[nodiscard]] std::coroutine_handle<> get_next_task() noexcept;
    void run_task(std::coroutine_handle<> handle) noexcept;
    [[nodiscard]] std::coroutine_handle<> try_steal() noexcept;
    void poll_io_when_idle();

    scheduler* scheduler_;
    size_t worker_id_;
    chase_lev_deque<void> queue_;      // Owner's local deque (SPMC)
    mpsc_queue<void> inbox_;           // External submissions (MPSC)
    std::thread thread_;
    std::atomic<bool> running_;
    std::atomic<size_t> tasks_executed_;
    std::atomic<bool> idle_{false};    // True when worker is waiting for work (for lazy wake)
    bool needs_sync_ = false;          // Whether current task needs memory synchronization
    wait_strategy strategy_;           // Configurable wait strategy
    std::unique_ptr<io::io_context> io_context_;  // Per-worker io_context
    
    static inline thread_local worker_thread* current_worker_ = nullptr;
};

} // namespace elio::runtime
