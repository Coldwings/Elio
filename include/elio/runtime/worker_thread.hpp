#pragma once

#include "chase_lev_deque.hpp"
#include "mpsc_queue.hpp"
#include <elio/coro/promise_base.hpp>
#include <elio/io/io_context.hpp>
#include <coroutine>
#include <thread>
#include <atomic>
#include <mutex>
#include <random>
#include <chrono>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <cerrno>

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
    worker_thread(scheduler* sched, size_t worker_id)
        : scheduler_(sched)
        , worker_id_(worker_id)
        , queue_()
        , inbox_()
        , running_(false)
        , tasks_executed_(0)
        , wake_fd_(-1)
        , wait_epoll_fd_(-1)
        , io_context_(std::make_unique<io::io_context>()) {
        
        // Create eventfd for wake-up notifications (non-blocking, semaphore mode)
        wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd_ < 0) {
            // Fall back to busy-wait if eventfd fails
            return;
        }
        
        // Create a private epoll instance for this worker's idle wait
        wait_epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (wait_epoll_fd_ < 0) {
            close(wake_fd_);
            wake_fd_ = -1;
            return;
        }
        
        // Register wake_fd with our private epoll
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = wake_fd_;
        if (epoll_ctl(wait_epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev) < 0) {
            close(wait_epoll_fd_);
            close(wake_fd_);
            wait_epoll_fd_ = -1;
            wake_fd_ = -1;
        }
    }

    ~worker_thread() {
        stop();
        if (wait_epoll_fd_ >= 0) {
            close(wait_epoll_fd_);
        }
        if (wake_fd_ >= 0) {
            close(wake_fd_);
        }
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
            // This avoids expensive eventfd write syscalls when worker is busy
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
        if (wake_fd_ >= 0) {
            uint64_t val = 1;
            // Write to eventfd to signal wake-up (ignore errors, best-effort)
            [[maybe_unused]] auto ret = ::write(wake_fd_, &val, sizeof(val));
        }
    }
    
    /// Get the eventfd for this worker (for external integration)
    [[nodiscard]] int wake_fd() const noexcept {
        return wake_fd_;
    }

private:
    void run();
    void drain_inbox() noexcept;
    [[nodiscard]] std::coroutine_handle<> get_next_task() noexcept;
    void run_task(std::coroutine_handle<> handle) noexcept;
    [[nodiscard]] std::coroutine_handle<> try_steal() noexcept;
    void poll_io_when_idle();
    
    /// Drain eventfd counter after wake-up
    void drain_wake_fd() noexcept {
        if (wake_fd_ >= 0) {
            uint64_t val;
            // Read to reset the eventfd counter (non-blocking, ignore errors)
            [[maybe_unused]] auto ret = ::read(wake_fd_, &val, sizeof(val));
        }
    }
    
    /// Wait for work with efficient blocking using epoll
    /// @param timeout_ms Maximum time to wait in milliseconds
    void wait_for_work(int timeout_ms) noexcept {
        if (wait_epoll_fd_ < 0) {
            // Fallback: no eventfd support, just yield
            std::this_thread::yield();
            return;
        }

        struct epoll_event ev;
        int ret = epoll_wait(wait_epoll_fd_, &ev, 1, timeout_ms);

        if (ret > 0) {
            // Got wake-up signal, drain the eventfd
            drain_wake_fd();
        }
        // ret == 0: timeout, ret < 0: error (EINTR) - all fine, just return
    }

    scheduler* scheduler_;
    size_t worker_id_;
    chase_lev_deque<void> queue_;      // Owner's local deque (SPMC)
    mpsc_queue<void> inbox_;           // External submissions (MPSC)
    std::thread thread_;
    std::atomic<bool> running_;
    std::atomic<size_t> tasks_executed_;
    std::atomic<bool> idle_{false};    // True when worker is waiting for work (for lazy wake)
    bool needs_sync_ = false;          // Whether current task needs memory synchronization
    int wake_fd_;                      // eventfd for wake-up notifications
    int wait_epoll_fd_;                // epoll fd for waiting on wake_fd
    std::unique_ptr<io::io_context> io_context_;  // Per-worker io_context
    
    static inline thread_local worker_thread* current_worker_ = nullptr;
};

} // namespace elio::runtime
