#pragma once

#include "chase_lev_deque.hpp"
#include "mpsc_queue.hpp"
#include "wait_strategy.hpp"
#include <elio/coro/promise_base.hpp>
#include <elio/io/io_context.hpp>
#include <elio/log/macros.hpp>
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
    friend class scheduler;

public:
    worker_thread(scheduler* sched, size_t worker_id,
                   wait_strategy strategy = wait_strategy::blocking())
        : scheduler_(sched)
        , worker_id_(worker_id)
        , queue_(std::make_unique<chase_lev_deque<void>>())
        , inbox_(std::make_unique<mpsc_queue<void>>())
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

    /// Schedule a task from external thread - pushes to lock-free MPSC inbox.
    /// Returns false without taking ownership if this worker is stopped or the
    /// inbox remains full after bounded retries.
    [[nodiscard]] bool schedule(std::coroutine_handle<> handle) {
        if (!handle) [[unlikely]] return false;

        enum class push_result {
            accepted,
            full,
            stopped,
        };

        auto try_push = [&]() -> push_result {
            std::lock_guard<std::mutex> lock(schedule_mutex_);
            if (!running_.load(std::memory_order_acquire)) {
                return push_result::stopped;
            }
            if (!inbox_->push(handle.address())) {
                return push_result::full;
            }
            // Always wake on cross-thread submit. The eventfd dedupes via its
            // counter — calling wake() on a busy worker just bumps the counter
            // and is consumed in the next poll cycle. The previous "lazy wake"
            // load on idle_ raced with the worker's relaxed idle_=false store,
            // missing wakes and producing 10 ms tail latency on weak hardware.
            wake();
            return push_result::accepted;
        };

        switch (try_push()) {
        case push_result::accepted:
            return true;
        case push_result::stopped:
            return false;
        case push_result::full:
            break;
        }

        // Slow path: inbox full, retry with exponential back-off
        // Bounded so a stalled / shutting-down worker can't make us spin forever.
        int backoff = 1;
        constexpr int max_total_retries = 1024;
        for (int total = 0; total < max_total_retries; ++total) {
            for (int i = 0; i < backoff; ++i) {
                #if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
                #else
                std::this_thread::yield();
                #endif
            }
            switch (try_push()) {
            case push_result::accepted:
                return true;
            case push_result::stopped:
                return false;
            case push_result::full:
                break;
            }
            backoff = std::min(backoff * 2, 1024);
            std::this_thread::yield();
        }

        ELIO_LOG_ERROR("worker {} schedule(): rejecting handle after {} "
                      "inbox retries exhausted",
                      worker_id_, max_total_retries);
        return false;
    }

    void schedule_or_destroy(std::coroutine_handle<> handle) {
        if (!schedule(handle) && handle) {
            handle.destroy();
        }
    }
    
    /// Schedule a task from owner thread - pushes directly to local deque
    void schedule_local(std::coroutine_handle<> handle) {
        if (!handle) [[unlikely]] return;
        queue_->push(handle.address());
    }

    [[nodiscard]] size_t tasks_executed() const noexcept {
        return tasks_executed_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t steals_executed() const noexcept {
        return steals_executed_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t queue_size() const noexcept {
        return queue_->size() + inbox_->size_approx();
    }

    [[nodiscard]] std::coroutine_handle<> steal_task() noexcept {
        // Don't allow stealing from stopped workers
        if (!running_.load(std::memory_order_acquire)) return nullptr;
        void* addr = queue_->steal();
        return addr ? std::coroutine_handle<>::from_address(addr) : nullptr;
    }

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_draining() const noexcept {
        return draining_.load(std::memory_order_acquire);
    }

    void enter_draining_mode() noexcept {
        draining_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        draining_.store(true, std::memory_order_release);
        wake();
    }

    std::thread detach_thread() noexcept {
        return std::move(thread_);
    }

    /// Check if worker is idle (waiting for work)
    [[nodiscard]] bool is_idle() const noexcept {
        return idle_.load(std::memory_order_relaxed);
    }

    /// Get the last time a task was executed
    [[nodiscard]] std::chrono::steady_clock::time_point last_task_time() const noexcept {
        return last_task_time_.load(std::memory_order_relaxed);
    }

    /// Update the last task execution time (called after task completes)
    void update_last_task_time() noexcept {
        last_task_time_.store(std::chrono::steady_clock::now(), std::memory_order_relaxed);
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
    void request_stop() noexcept;

    void leave_draining_mode() noexcept {
        draining_deadline_ = {};
        draining_.store(false, std::memory_order_release);
    }

    void run();
    void drain_inbox() noexcept;
    void run_or_redistribute_retiring_task(scheduler* sched,
                                           std::coroutine_handle<> handle) noexcept;
    [[nodiscard]] std::coroutine_handle<> get_next_task() noexcept;
    void run_task(std::coroutine_handle<> handle) noexcept;
    [[nodiscard]] std::coroutine_handle<> try_steal() noexcept;
    void poll_io_when_idle();

    scheduler* scheduler_;
    size_t worker_id_;
    std::unique_ptr<chase_lev_deque<void>> queue_;      // Owner's local deque (SPMC)
    std::unique_ptr<mpsc_queue<void>> inbox_;           // External submissions (MPSC)
    std::thread thread_;
    std::atomic<bool> running_;
    std::mutex schedule_mutex_;
    std::atomic<bool> draining_{false};
    std::chrono::steady_clock::time_point draining_deadline_{};
    // Hot-write fields (owner thread writes per task) — isolated cache line
    alignas(64) std::atomic<size_t> tasks_executed_;
    std::atomic<size_t> steals_executed_{0};

    // Idle flag (owner writes, other threads read) — isolated cache line
    alignas(64) std::atomic<bool> idle_{false};

    // Slow-update fields — isolated cache line
    alignas(64) std::atomic<std::chrono::steady_clock::time_point> last_task_time_{std::chrono::steady_clock::now()};
    bool needs_sync_ = false;          // Whether current task needs memory synchronization
    wait_strategy strategy_;           // Configurable wait strategy
    std::unique_ptr<io::io_context> io_context_;  // Per-worker io_context
    
    static inline thread_local worker_thread* current_worker_ = nullptr;
};

} // namespace elio::runtime
