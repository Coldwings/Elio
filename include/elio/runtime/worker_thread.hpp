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
#include <deque>
#include <mutex>
#include <random>
#include <chrono>

namespace elio::runtime {

class scheduler;

#ifdef ELIO_RUNTIME_TEST_HOOKS
namespace detail {
inline std::atomic<bool> pause_overflow_transfer_for_test{false};
inline std::atomic<bool> overflow_transfer_paused_for_test{false};
inline std::atomic<bool> pause_queue_snapshot_for_test{false};
inline std::atomic<bool> queue_snapshot_paused_for_test{false};
inline std::atomic<bool> queue_transfer_waiting_for_test{false};
}  // namespace detail
#endif

/// Worker thread that executes tasks from a local queue
/// and steals from other workers when idle
/// 
/// Uses bounded queues on the scheduling fast path:
/// - MPSC inbox: lock-free ring used behind the worker lifecycle guard
/// - Chase-Lev deque: lock-free SPMC for owner ops and work stealing
/// Rare sustained bursts spill into a mutex-protected overflow queue instead
/// of rejecting accepted worker-bound resumptions.
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
        , io_context_(io::io_context::make_worker_owned(worker_id)) {
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

    /// Schedule a task from an external thread. The lock-free MPSC inbox is
    /// the fast path; sustained bursts spill into a locked overflow queue so
    /// live workers do not reject borrowed resume handles.
    /// Returns false without taking ownership if this worker is stopped or the
    /// unpublished overflow insertion cannot allocate.
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

        // Preserve progress without making the hot queue dynamically sized.
        // schedule_mutex_ closes the race with request_stop(); overflow_mutex_
        // protects the slow-path queue from its single worker consumer.
        try {
            std::lock_guard<std::mutex> schedule_lock(schedule_mutex_);
            if (!running_.load(std::memory_order_acquire)) {
                return false;
            }
            {
                std::lock_guard<std::mutex> overflow_lock(overflow_mutex_);
                overflow_.push_back(handle.address());
                overflow_size_.fetch_add(1, std::memory_order_release);
            }
            wake();
        } catch (...) {
            // The handle was not published if overflow allocation failed.
            return false;
        }
        return true;
    }

#ifdef ELIO_RUNTIME_TEST_HOOKS
    [[nodiscard]] bool schedule_overflow_for_test(std::coroutine_handle<> handle) {
        if (!handle) return false;
        {
            std::lock_guard<std::mutex> schedule_lock(schedule_mutex_);
            if (!running_.load(std::memory_order_acquire)) {
                return false;
            }
            std::lock_guard<std::mutex> overflow_lock(overflow_mutex_);
            overflow_.push_back(handle.address());
            overflow_size_.fetch_add(1, std::memory_order_release);
        }
        wake();
        return true;
    }
#endif

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
        std::unique_lock<std::mutex> transfer_lock(transfer_mutex_,
                                                   std::try_to_lock);
        if (!transfer_lock.owns_lock()) {
            // A source-to-deque transfer is in progress. Conservatively
            // report pending work instead of blocking an accounting reader.
            return 1;
        }
        size_t total = queue_->size();
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (detail::pause_queue_snapshot_for_test.load(
                std::memory_order_acquire)) {
            detail::queue_snapshot_paused_for_test.store(
                true, std::memory_order_release);
            detail::queue_snapshot_paused_for_test.notify_all();
            while (detail::pause_queue_snapshot_for_test.load(
                std::memory_order_acquire)) {
                detail::pause_queue_snapshot_for_test.wait(
                    true, std::memory_order_acquire);
            }
            detail::queue_snapshot_paused_for_test.store(
                false, std::memory_order_release);
        }
#endif
        total += inbox_->size_approx();
        total += overflow_size_.load(std::memory_order_acquire);
        return total;
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

    [[nodiscard]] const io::io_context& io_context() const noexcept {
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
        draining_.store(false, std::memory_order_release);
    }

    void run();
    void drain_inbox() noexcept;
    [[nodiscard]] bool run_or_redistribute_retiring_task(
        scheduler* sched, std::coroutine_handle<> handle) noexcept;
    [[nodiscard]] std::coroutine_handle<> get_next_task() noexcept;
    void run_task(std::coroutine_handle<> handle) noexcept;
    [[nodiscard]] std::coroutine_handle<> try_steal() noexcept;
    void poll_io_when_idle();

    scheduler* scheduler_;
    size_t worker_id_;
    std::unique_ptr<chase_lev_deque<void>> queue_;      // Owner's local deque (SPMC)
    std::unique_ptr<mpsc_queue<void>> inbox_;           // External submissions (MPSC)
    std::deque<void*> overflow_;                        // Rare full-inbox fallback
    std::thread thread_;
    std::atomic<bool> running_;
    std::mutex schedule_mutex_;
    std::mutex overflow_mutex_;
    // Serializes external-queue transfers with non-blocking accounting
    // snapshots. It is not used by the local scheduling fast path.
    mutable std::mutex transfer_mutex_;
    // Includes entries swapped into a worker-local transfer batch until every
    // handle in that batch has been published to queue_. This may briefly
    // overcount work, but it must never let idle detection miss accepted work.
    std::atomic<size_t> overflow_size_{0};
    std::atomic<bool> draining_{false};
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
