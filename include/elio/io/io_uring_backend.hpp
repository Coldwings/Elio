#pragma once

#include "io_backend.hpp"
#include <elio/log/macros.hpp>

#if ELIO_HAS_IO_URING

#include <liburing.h>
#include <sys/eventfd.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <atomic>

namespace elio::io {

/// io_uring backend implementation
/// 
/// Provides high-performance asynchronous I/O on Linux 5.1+
/// Features:
/// - Submission queue batching
/// - Zero-copy operations (when available)
/// - Efficient completion handling
class io_uring_backend : public io_backend {
public:
    /// Configuration options
    struct config {
        uint32_t queue_depth = 256;     ///< SQ/CQ depth
        uint32_t flags = 0;             ///< io_uring_setup flags
        bool sq_poll = false;           ///< Enable SQ polling (requires privileges)
    };
    
    /// Constructor with default configuration
    io_uring_backend() : io_uring_backend(config{}) {}
    
    /// Constructor with custom configuration
    explicit io_uring_backend(const config& cfg) 
        : pending_ops_(0) {
        
        struct io_uring_params params = {};
        params.flags = cfg.flags;
        
        if (cfg.sq_poll) {
            params.flags |= IORING_SETUP_SQPOLL;
            params.sq_thread_idle = 2000;  // 2 second idle timeout
        }
        
        int ret = io_uring_queue_init_params(cfg.queue_depth, &ring_, &params);
        if (ret < 0) {
            throw std::runtime_error(
                std::string("io_uring_queue_init failed: ") + strerror(-ret)
            );
        }
        
        ELIO_LOG_INFO("io_uring_backend initialized (queue_depth={}, flags=0x{:x})",
                      cfg.queue_depth, params.flags);

        // Create eventfd for cross-thread wake notifications
        wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd_ < 0) {
            io_uring_queue_exit(&ring_);
            throw std::runtime_error(
                std::string("eventfd creation failed: ") + strerror(errno)
            );
        }

        // Submit initial poll_add to watch wake_fd_
        submit_wake_poll();
        io_uring_submit(&ring_);
    }
    
    /// Destructor
    ~io_uring_backend() override {
        // Brief wait for pending operations - don't block forever
        // io_uring_queue_exit will handle cleanup of any remaining ops
        int retries = 10;  // Max ~100ms wait
        while (pending_ops_.load(std::memory_order_relaxed) > 0 && retries-- > 0) {
            poll(std::chrono::milliseconds(10));
        }
        
        if (pending_ops_.load(std::memory_order_relaxed) > 0) {
            ELIO_LOG_WARNING("io_uring_backend destroyed with {} pending operations",
                            pending_ops_.load(std::memory_order_relaxed));
        }
        
        if (wake_fd_ >= 0) {
            ::close(wake_fd_);
            wake_fd_ = -1;
        }

        io_uring_queue_exit(&ring_);
        ELIO_LOG_INFO("io_uring_backend destroyed");
    }
    
    // Non-copyable, non-movable
    io_uring_backend(const io_uring_backend&) = delete;
    io_uring_backend& operator=(const io_uring_backend&) = delete;
    io_uring_backend(io_uring_backend&&) = delete;
    io_uring_backend& operator=(io_uring_backend&&) = delete;
    
    /// Prepare an I/O operation
    bool prepare(const io_request& req) override {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            ELIO_LOG_WARNING("io_uring SQ full, cannot prepare operation");
            return false;
        }
        
        // Encode the awaiter handle as user_data
        io_uring_sqe_set_data(sqe, req.awaiter.address());
        
        switch (req.op) {
            case io_op::read:
                if (req.offset >= 0) {
                    io_uring_prep_read(sqe, req.fd, req.buffer, 
                                       static_cast<unsigned>(req.length), 
                                       static_cast<__u64>(req.offset));
                } else {
                    io_uring_prep_read(sqe, req.fd, req.buffer, 
                                       static_cast<unsigned>(req.length), 0);
                }
                break;
                
            case io_op::write:
                if (req.offset >= 0) {
                    io_uring_prep_write(sqe, req.fd, req.buffer, 
                                        static_cast<unsigned>(req.length), 
                                        static_cast<__u64>(req.offset));
                } else {
                    io_uring_prep_write(sqe, req.fd, req.buffer, 
                                        static_cast<unsigned>(req.length), 0);
                }
                break;
                
            case io_op::readv:
                io_uring_prep_readv(sqe, req.fd, req.iovecs, 
                                    static_cast<unsigned>(req.iovec_count),
                                    static_cast<__u64>(req.offset >= 0 ? req.offset : 0));
                break;
                
            case io_op::writev:
                io_uring_prep_writev(sqe, req.fd, req.iovecs, 
                                     static_cast<unsigned>(req.iovec_count),
                                     static_cast<__u64>(req.offset >= 0 ? req.offset : 0));
                break;
                
            case io_op::accept:
                io_uring_prep_accept(sqe, req.fd, req.addr, req.addrlen, 
                                     req.socket_flags);
                break;
                
            case io_op::connect:
                io_uring_prep_connect(sqe, req.fd, req.addr, 
                                      req.addrlen ? *req.addrlen : 0);
                break;
                
            case io_op::recv:
                io_uring_prep_recv(sqe, req.fd, req.buffer, req.length, 
                                   req.socket_flags);
                break;
                
            case io_op::send:
                io_uring_prep_send(sqe, req.fd, req.buffer, req.length, 
                                   req.socket_flags);
                break;
                
            case io_op::close:
                io_uring_prep_close(sqe, req.fd);
                break;
                
            case io_op::timeout: {
                // Timeout in nanoseconds
                // Store timespec in user_data area of the awaiter to avoid shared state
                // The awaiter must have a ts_ member for this to work
                auto ns = static_cast<int64_t>(req.length);
                auto* ts = static_cast<__kernel_timespec*>(req.timeout_ts);
                if (ts) {
                    ts->tv_sec = ns / 1000000000LL;
                    ts->tv_nsec = ns % 1000000000LL;
                    io_uring_prep_timeout(sqe, ts, 0, 0);
                }
                break;
            }
                
            case io_op::cancel:
                io_uring_prep_cancel(sqe, req.user_data, 0);
                break;
                
            case io_op::poll_read:
                io_uring_prep_poll_add(sqe, req.fd, POLLIN);
                break;
                
            case io_op::poll_write:
                io_uring_prep_poll_add(sqe, req.fd, POLLOUT);
                break;
                
            default:
                ELIO_LOG_ERROR("Unknown io_op: {}", static_cast<int>(req.op));
                return false;
        }
        
        pending_ops_.fetch_add(1, std::memory_order_relaxed);
        ELIO_LOG_DEBUG("Prepared io_op::{} on fd={}", 
                       static_cast<int>(req.op), req.fd);
        return true;
    }
    
    /// Submit all prepared operations
    int submit() override {
        // Fast path: if no pending submissions, skip syscall
        size_t pending = io_uring_sq_ready(&ring_);
        if (pending == 0) {
            return 0;
        }

        int submitted = io_uring_submit(&ring_);
        if (submitted < 0) {
            ELIO_LOG_ERROR("io_uring_submit failed: {}", strerror(-submitted));
            return submitted;
        }

        ELIO_LOG_DEBUG("Submitted {} operations", submitted);
        return submitted;
    }
    
    /// Poll for completed operations
    int poll(std::chrono::milliseconds timeout) override {
        // Auto-submit any pending operations before polling
        // This enables batching: multiple prepares followed by one submit
        if (io_uring_sq_ready(&ring_) > 0) {
            io_uring_submit(&ring_);
        }

        struct io_uring_cqe* cqe = nullptr;
        int completions = 0;
        std::vector<deferred_resume_entry> deferred_resumes;
        
        if (timeout.count() == 0) {
            // Non-blocking: peek for available CQEs
            while (io_uring_peek_cqe(&ring_, &cqe) == 0 && cqe) {
                process_completion(cqe, &deferred_resumes);
                io_uring_cqe_seen(&ring_, cqe);
                completions++;
                cqe = nullptr;
            }
        } else if (timeout.count() < 0) {
            // Blocking: wait for at least one CQE
            int ret = io_uring_wait_cqe(&ring_, &cqe);
            if (ret == 0 && cqe) {
                process_completion(cqe, &deferred_resumes);
                io_uring_cqe_seen(&ring_, cqe);
                completions++;
                
                // Drain any additional ready CQEs
                cqe = nullptr;
                while (io_uring_peek_cqe(&ring_, &cqe) == 0 && cqe) {
                    process_completion(cqe, &deferred_resumes);
                    io_uring_cqe_seen(&ring_, cqe);
                    completions++;
                    cqe = nullptr;
                }
            }
        } else {
            // Timed wait
            struct __kernel_timespec ts;
            ts.tv_sec = timeout.count() / 1000;
            ts.tv_nsec = (timeout.count() % 1000) * 1000000;
            
            int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
            if (ret == 0 && cqe) {
                process_completion(cqe, &deferred_resumes);
                io_uring_cqe_seen(&ring_, cqe);
                completions++;
                
                // Drain any additional ready CQEs
                cqe = nullptr;
                while (io_uring_peek_cqe(&ring_, &cqe) == 0 && cqe) {
                    process_completion(cqe, &deferred_resumes);
                    io_uring_cqe_seen(&ring_, cqe);
                    completions++;
                    cqe = nullptr;
                }
            }
        }
        
        // Resume coroutines after processing all completions
        // Each coroutine gets its correct result via deferred_resume_entry
        resume_deferred(deferred_resumes);
        
        if (completions > 0) {
            ELIO_LOG_DEBUG("Processed {} completions", completions);
        }
        
        return completions;
    }
    
    /// Check if there are pending operations
    bool has_pending() const noexcept override {
        return pending_ops_.load(std::memory_order_relaxed) > 0;
    }
    
    /// Get the number of pending operations
    size_t pending_count() const noexcept override {
        return pending_ops_.load(std::memory_order_relaxed);
    }
    
    /// Cancel a pending operation
    bool cancel(void* user_data) override {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            return false;
        }
        
        io_uring_prep_cancel(sqe, user_data, 0);
        io_uring_sqe_set_data(sqe, nullptr);  // No awaiter for cancel itself
        
        pending_ops_.fetch_add(1, std::memory_order_relaxed);
        
        // Submit immediately - cancel must be acted upon right away
        io_uring_submit(&ring_);
        return true;
    }
    
    /// Check if io_uring is available on this system
    static bool is_available() noexcept {
        struct io_uring ring;
        int ret = io_uring_queue_init(1, &ring, 0);
        if (ret == 0) {
            io_uring_queue_exit(&ring);
            return true;
        }
        return false;
    }

    /// Override to indicate this is an io_uring backend
    bool is_io_uring() const noexcept override { return true; }

    void notify() override {
        uint64_t val = 1;
        ssize_t ret = ::write(wake_fd_, &val, sizeof(val));
        if (ret < 0 && errno != EAGAIN) {
            ELIO_LOG_WARNING("eventfd write failed: {}", strerror(errno));
        }
    }

    void drain_notify() override {
        uint64_t val;
        ssize_t ret = ::read(wake_fd_, &val, sizeof(val));
        if (ret < 0 && errno != EAGAIN) {
            ELIO_LOG_WARNING("eventfd read failed: {}", strerror(errno));
        }
    }

private:
    /// Deferred resume entry - stores handle with its result
    struct deferred_resume_entry {
        std::coroutine_handle<> handle;
        io_result result;
    };
    
    void process_completion(struct io_uring_cqe* cqe,
                           std::vector<deferred_resume_entry>* deferred_resumes = nullptr) {
        void* user_data = io_uring_cqe_get_data(cqe);

        // Check for wake notification sentinel
        if (user_data == reinterpret_cast<void*>(WAKE_SENTINEL)) {
            drain_notify();
            submit_wake_poll();
            return;
        }

        pending_ops_.fetch_sub(1, std::memory_order_relaxed);

        if (!user_data) {
            // Cancel operation or internal operation, no awaiter to resume
            return;
        }
        
        // Store result in promise and resume coroutine
        auto handle = std::coroutine_handle<>::from_address(user_data);
        io_result result{cqe->res, cqe->flags};
        
        ELIO_LOG_DEBUG("Completing operation: result={}, flags={}", 
                       cqe->res, cqe->flags);
        
        if (handle && !handle.done()) {
            if (deferred_resumes) {
                deferred_resumes->push_back({handle, result});
            } else {
                last_result_ = result;
                handle.resume();
            }
        }
    }
    
    /// Resume collected coroutine handles (call outside of lock)
    static void resume_deferred(std::vector<deferred_resume_entry>& entries) {
        for (auto& entry : entries) {
            if (entry.handle && !entry.handle.done()) {
                last_result_ = entry.result;
                entry.handle.resume();
            }
        }
    }

    /// Submit a poll_add SQE to watch the wake eventfd
    void submit_wake_poll() {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (sqe) {
            io_uring_prep_poll_add(sqe, wake_fd_, POLLIN);
            io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(WAKE_SENTINEL));
            // Don't count this as a pending user operation
        } else {
            ELIO_LOG_WARNING("Failed to submit wake poll: SQ full");
        }
    }
    
public:
    /// Get the last completion result (thread-local)
    /// Used by awaitables to retrieve their result
    static io_result get_last_result() noexcept {
        return last_result_;
    }
    
private:
    struct io_uring ring_;                     ///< io_uring instance
    std::atomic<size_t> pending_ops_;          ///< Number of pending operations
    int wake_fd_ = -1;  ///< eventfd for cross-thread wake-up
    /// Sentinel user_data to identify wake notifications in CQE
    static constexpr uintptr_t WAKE_SENTINEL = 1;

    static inline thread_local io_result last_result_{};
};

} // namespace elio::io

#else // !ELIO_HAS_IO_URING

namespace elio::io {

/// Stub implementation when io_uring is not available
class io_uring_backend : public io_backend {
public:
    io_uring_backend() {
        throw std::runtime_error("io_uring not available on this system");
    }
    
    bool prepare(const io_request&) override { return false; }
    int submit() override { return -1; }
    int poll(std::chrono::milliseconds) override { return 0; }
    bool has_pending() const noexcept override { return false; }
    size_t pending_count() const noexcept override { return 0; }
    bool cancel(void*) override { return false; }
    
    static bool is_available() noexcept { return false; }
    static io_result get_last_result() noexcept { return {}; }

    void notify() override {}
    void drain_notify() override {}
};

} // namespace elio::io

#endif // ELIO_HAS_IO_URING
