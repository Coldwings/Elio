#pragma once

#include "io_backend.hpp"
#include <elio/log/macros.hpp>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <queue>
#include <chrono>

namespace elio::io {

/// epoll backend implementation
/// 
/// Provides efficient I/O multiplexing on Linux
/// Used as a fallback when io_uring is not available
/// 
/// Features:
/// - Edge-triggered mode for efficiency
/// - Scales well with many file descriptors
/// - Compatible with older Linux kernels
class epoll_backend : public io_backend {
public:
    /// Configuration options
    struct config {
        size_t max_events = 256;     ///< Max events per poll
    };
    
    /// Constructor with default configuration
    epoll_backend() : epoll_backend(config{}) {}
    
    /// Constructor with custom configuration
    explicit epoll_backend(const config& cfg)
        : events_(cfg.max_events) {
        
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            throw std::runtime_error(
                std::string("epoll_create1 failed: ") + strerror(errno)
            );
        }
        
        ELIO_LOG_INFO("epoll_backend initialized (max_events={})", cfg.max_events);

        // Create eventfd for cross-thread wake notifications
        wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd_ < 0) {
            ::close(epoll_fd_);
            throw std::runtime_error(
                std::string("eventfd creation failed: ") + strerror(errno)
            );
        }

        // Register wake_fd_ with epoll for read events
        struct epoll_event wake_ev;
        wake_ev.events = EPOLLIN;
        wake_ev.data.fd = wake_fd_;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &wake_ev) < 0) {
            ::close(wake_fd_);
            ::close(epoll_fd_);
            throw std::runtime_error(
                std::string("epoll_ctl for wake_fd failed: ") + strerror(errno)
            );
        }
    }
    
    /// Destructor
    ~epoll_backend() override {
        // Collect handles to resume
        std::vector<deferred_resume_entry> deferred_resumes;
        
        for (auto& [fd, state] : fd_states_) {
            for (auto& op : state.pending_ops) {
                if (op.awaiter && !op.awaiter.done()) {
                    deferred_resumes.push_back({op.awaiter, io_result{-ECANCELED, 0}});
                }
            }
        }
        fd_states_.clear();
        
        // Cancel all pending timers
        while (!timer_queue_.empty()) {
            auto& entry = timer_queue_.top();
            if (entry.awaiter && !entry.awaiter.done()) {
                deferred_resumes.push_back({entry.awaiter, io_result{-ECANCELED, 0}});
            }
            timer_queue_.pop();
        }
        
        // Resume coroutines
        resume_deferred(deferred_resumes);

        if (wake_fd_ >= 0) {
            ::close(wake_fd_);
        }

        if (epoll_fd_ >= 0) {
            close(epoll_fd_);
        }
        
        ELIO_LOG_INFO("epoll_backend destroyed");
    }
    
    // Non-copyable, non-movable
    epoll_backend(const epoll_backend&) = delete;
    epoll_backend& operator=(const epoll_backend&) = delete;
    epoll_backend(epoll_backend&&) = delete;
    epoll_backend& operator=(epoll_backend&&) = delete;
    
    /// Prepare an I/O operation
    bool prepare(const io_request& req) override {
        pending_operation op;
        op.req = req;
        op.awaiter = req.awaiter;
        
        // For epoll, we need to determine what events to watch for
        uint32_t events = EPOLLET;  // Edge-triggered
        
        switch (req.op) {
            case io_op::read:
            case io_op::readv:
            case io_op::recv:
            case io_op::accept:
            case io_op::poll_read:
                events |= EPOLLIN;
                break;
                
            case io_op::write:
            case io_op::writev:
            case io_op::send:
            case io_op::connect:
            case io_op::poll_write:
                events |= EPOLLOUT;
                break;
                
            case io_op::close:
                // Close operations are executed synchronously
                op.synchronous = true;
                break;
                
            case io_op::timeout: {
                // Use timer queue for timeout operations
                int64_t timeout_ns = static_cast<int64_t>(req.length);
                auto deadline = std::chrono::steady_clock::now() + 
                                std::chrono::nanoseconds(timeout_ns);
                
                timer_queue_.push(timer_entry{deadline, req.awaiter});
                pending_count_++;
                
                ELIO_LOG_DEBUG("Prepared timeout: {}ns", timeout_ns);
                return true;
            }
                
            case io_op::cancel:
            case io_op::none:
                return false;
        }
        
        // Get or create fd state
        auto& state = fd_states_[req.fd];
        bool is_sync = op.synchronous;
        state.pending_ops.push_back(std::move(op));
        
        if (!is_sync) {
            // Register with epoll
            state.events |= events;
            
            struct epoll_event ev;
            ev.events = state.events;
            ev.data.fd = req.fd;
            
            int ret;
            if (state.registered) {
                ret = epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, req.fd, &ev);
            } else {
                ret = epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, req.fd, &ev);
                if (ret == 0) {
                    state.registered = true;
                }
            }
            
            if (ret < 0 && errno != EEXIST) {
                ELIO_LOG_WARNING("epoll_ctl failed for fd {}: {}", 
                                 req.fd, strerror(errno));
            }
        }
        
        pending_count_++;
        ELIO_LOG_DEBUG("Prepared io_op::{} on fd={}", 
                       static_cast<int>(req.op), req.fd);
        return true;
    }
    
    /// Submit all prepared operations
    /// For epoll, operations are "submitted" when they're added
    /// This just executes any synchronous operations
    int submit() override {
        int submitted = 0;
        
        // Execute synchronous operations (like close)
        for (auto& [fd, state] : fd_states_) {
            auto it = state.pending_ops.begin();
            while (it != state.pending_ops.end()) {
                if (it->synchronous) {
                    execute_sync_op(*it);
                    it = state.pending_ops.erase(it);
                    pending_count_--;
                    submitted++;
                } else {
                    ++it;
                }
            }
        }
        
        ELIO_LOG_DEBUG("Submitted {} synchronous operations", submitted);
        return submitted;
    }
    
    /// Poll for completed operations
    int poll(std::chrono::milliseconds timeout) override {
        int timeout_ms = static_cast<int>(timeout.count());
        if (timeout.count() < 0) {
            timeout_ms = -1;  // Block indefinitely
        }
        
        // Adjust timeout based on earliest timer deadline
        if (!timer_queue_.empty()) {
            auto now = std::chrono::steady_clock::now();
            auto earliest = timer_queue_.top().deadline;
            if (earliest <= now) {
                timeout_ms = 0;  // Timer already expired
            } else {
                auto timer_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
                    earliest - now).count();
                if (timeout_ms < 0 || timer_timeout < timeout_ms) {
                    timeout_ms = static_cast<int>(timer_timeout);
                }
            }
        }
        
        int nfds = epoll_wait(epoll_fd_, events_.data(), 
                              static_cast<int>(events_.size()), timeout_ms);
        
        if (nfds < 0) {
            if (errno == EINTR) {
                return 0;  // Interrupted, retry
            }
            ELIO_LOG_ERROR("epoll_wait failed: {}", strerror(errno));
            return -1;
        }
        
        int completions = 0;
        
        // Collect handles to resume after processing all completions
        std::vector<deferred_resume_entry> deferred_resumes;
        
        // Process expired timers
        auto now = std::chrono::steady_clock::now();
        while (!timer_queue_.empty() && timer_queue_.top().deadline <= now) {
            auto entry = timer_queue_.top();
            timer_queue_.pop();
            
            io_result result{0, 0};  // Timeout completed successfully
            
            if (entry.awaiter && !entry.awaiter.done()) {
                deferred_resumes.push_back({entry.awaiter, result});
            }
            
            pending_count_--;
            completions++;
            
            ELIO_LOG_DEBUG("Timer expired");
        }
        
        // Process epoll events
        for (int i = 0; i < nfds; ++i) {
            int fd = events_[i].data.fd;

            // Wake notification event - drain eventfd to prevent busy-loop
            // (level-triggered epoll will keep firing until eventfd counter is zero)
            if (fd == wake_fd_) {
                drain_notify();
                continue;
            }

            uint32_t revents = events_[i].events;
            
            auto it = fd_states_.find(fd);
            if (it == fd_states_.end()) {
                continue;
            }
            
            fd_state& state = it->second;
                
                // Process pending operations for this fd
                auto op_it = state.pending_ops.begin();
                while (op_it != state.pending_ops.end()) {
                    bool ready = false;
                    
                    switch (op_it->req.op) {
                        case io_op::read:
                        case io_op::readv:
                        case io_op::recv:
                        case io_op::accept:
                        case io_op::poll_read:
                            ready = (revents & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0;
                            break;
                            
                        case io_op::write:
                        case io_op::writev:
                        case io_op::send:
                        case io_op::connect:
                        case io_op::poll_write:
                            ready = (revents & (EPOLLOUT | EPOLLHUP | EPOLLERR)) != 0;
                            break;
                            
                        default:
                            break;
                    }
                    
                    if (ready) {
                        execute_async_op(*op_it, revents, &deferred_resumes);
                        op_it = state.pending_ops.erase(op_it);
                        pending_count_--;
                        completions++;
                    } else {
                        ++op_it;
                    }
                }
                
                // Update epoll registration if no more pending ops for this fd
                if (state.pending_ops.empty() && state.registered) {
                    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                    state.registered = false;
                    state.events = 0;
                }
            }
        
        // Resume coroutines after processing all completions
        resume_deferred(deferred_resumes);
        
        if (completions > 0) {
            ELIO_LOG_DEBUG("Processed {} completions", completions);
        }
        
        return completions;
    }
    
    /// Check if there are pending operations
    bool has_pending() const noexcept override {
        return pending_count_ > 0;
    }
    
    /// Get the number of pending operations
    size_t pending_count() const noexcept override {
        return pending_count_;
    }
    
    /// Cancel a pending operation
    bool cancel(void* user_data) override {
        deferred_resume_entry to_resume{};
        bool found_entry = false;
        
        // Search in fd_states first
        for (auto& [fd, state] : fd_states_) {
            auto it = std::find_if(state.pending_ops.begin(), 
                                    state.pending_ops.end(),
                                    [user_data](const pending_operation& op) {
                                        return op.awaiter.address() == user_data;
                                    });
            
            if (it != state.pending_ops.end()) {
                // Collect handle for deferred resumption
                if (it->awaiter && !it->awaiter.done()) {
                    to_resume = {it->awaiter, io_result{-ECANCELED, 0}};
                    found_entry = true;
                }
                state.pending_ops.erase(it);
                pending_count_--;
                
                // Cleanup if no more pending ops
                if (state.pending_ops.empty() && state.registered) {
                    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                    state.registered = false;
                    state.events = 0;
                }
                goto found;
            }
        }
        
        // Search in timer queue - need to rebuild queue without the cancelled entry
        if (!timer_queue_.empty()) {
            std::vector<timer_entry> remaining;
            while (!timer_queue_.empty()) {
                auto entry = timer_queue_.top();
                timer_queue_.pop();
                if (entry.awaiter.address() == user_data) {
                    if (entry.awaiter && !entry.awaiter.done()) {
                        to_resume = {entry.awaiter, io_result{-ECANCELED, 0}};
                        found_entry = true;
                    }
                    pending_count_--;
                    // Don't add back to remaining
                } else {
                    remaining.push_back(entry);
                }
            }
            // Rebuild queue
            for (auto& e : remaining) {
                timer_queue_.push(std::move(e));
            }
        }
    found:
        
        // Resume the cancelled coroutine
        if (found_entry && to_resume.handle && !to_resume.handle.done()) {
            last_result_ = to_resume.result;
            to_resume.handle.resume();
            return true;
        }
        
        return false;
    }
    
    /// Check if epoll is available (always true on Linux)
    static bool is_available() noexcept {
        return true;
    }
    
    /// Get the last completion result (thread-local)
    static io_result get_last_result() noexcept {
        return last_result_;
    }

    void notify() override {
        uint64_t val = 1;
        ::write(wake_fd_, &val, sizeof(val));
    }

    void drain_notify() override {
        uint64_t val;
        ::read(wake_fd_, &val, sizeof(val));
    }

private:
    struct pending_operation {
        io_request req;
        std::coroutine_handle<> awaiter;
        bool synchronous = false;
    };
    
    struct fd_state {
        std::vector<pending_operation> pending_ops;
        uint32_t events = 0;
        bool registered = false;
    };
    
    /// Timer entry for the timer queue
    struct timer_entry {
        std::chrono::steady_clock::time_point deadline;
        std::coroutine_handle<> awaiter;
        
        /// Comparison for min-heap (earliest deadline first)
        bool operator>(const timer_entry& other) const {
            return deadline > other.deadline;
        }
    };
    
    /// Deferred resume entry - stores handle with its result
    struct deferred_resume_entry {
        std::coroutine_handle<> handle;
        io_result result;
    };
    
    /// Min-heap priority queue for timers
    using timer_queue_t = std::priority_queue<timer_entry, 
                                               std::vector<timer_entry>,
                                               std::greater<timer_entry>>;
    
    void execute_sync_op(pending_operation& op) {
        int result = 0;
        
        switch (op.req.op) {
            case io_op::close:
                result = ::close(op.req.fd);
                if (result < 0) {
                    result = -errno;
                }
                break;
                
            default:
                result = -ENOTSUP;
                break;
        }
        
        last_result_ = io_result{result, 0};
        
        if (op.awaiter && !op.awaiter.done()) {
            op.awaiter.resume();
        }
    }
    
    /// Execute async I/O operation
    /// @param op The pending operation to execute
    /// @param revents The epoll events that triggered this operation
    /// @param deferred_resumes If non-null, collect handle+result for later resumption (avoids deadlock)
    void execute_async_op(pending_operation& op, uint32_t revents,
                          std::vector<deferred_resume_entry>* deferred_resumes = nullptr) {
        int result = 0;
        
        // Check for errors first
        if (revents & EPOLLERR) {
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(op.req.fd, SOL_SOCKET, SO_ERROR, &error, &len);
            result = -error;
        } else if (revents & EPOLLHUP) {
            // For reads, HUP means EOF (result = 0)
            // For other operations, it's an error
            switch (op.req.op) {
                case io_op::read:
                case io_op::recv:
                    result = 0;  // EOF
                    break;
                default:
                    result = -EPIPE;
                    break;
            }
        } else {
            // Execute the actual I/O operation
            switch (op.req.op) {
                case io_op::read:
                    if (op.req.offset >= 0) {
                        result = static_cast<int>(pread(op.req.fd, op.req.buffer, 
                                                        op.req.length, op.req.offset));
                    } else {
                        result = static_cast<int>(read(op.req.fd, op.req.buffer, 
                                                       op.req.length));
                    }
                    break;
                    
                case io_op::write:
                    if (op.req.offset >= 0) {
                        result = static_cast<int>(pwrite(op.req.fd, op.req.buffer, 
                                                         op.req.length, op.req.offset));
                    } else {
                        result = static_cast<int>(write(op.req.fd, op.req.buffer, 
                                                        op.req.length));
                    }
                    break;
                    
                case io_op::readv:
                    result = static_cast<int>(readv(op.req.fd, op.req.iovecs, 
                                                    static_cast<int>(op.req.iovec_count)));
                    break;
                    
                case io_op::writev:
                    result = static_cast<int>(writev(op.req.fd, op.req.iovecs, 
                                                     static_cast<int>(op.req.iovec_count)));
                    break;
                    
                case io_op::recv:
                    result = static_cast<int>(recv(op.req.fd, op.req.buffer, 
                                                   op.req.length, op.req.socket_flags));
                    break;
                    
                case io_op::send:
                    result = static_cast<int>(send(op.req.fd, op.req.buffer, 
                                                   op.req.length, op.req.socket_flags));
                    break;
                    
                case io_op::accept:
                    result = accept4(op.req.fd, op.req.addr, op.req.addrlen, 
                                     op.req.socket_flags | SOCK_CLOEXEC);
                    break;
                    
                case io_op::connect: {
                    // For connect, check if it succeeded
                    int error = 0;
                    socklen_t len = sizeof(error);
                    if (getsockopt(op.req.fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0) {
                        result = error == 0 ? 0 : -error;
                    } else {
                        result = -errno;
                    }
                    break;
                }
                
                case io_op::poll_read:
                case io_op::poll_write:
                    // Poll just returns success when the socket is ready
                    result = 0;
                    break;
                    
                default:
                    result = -ENOTSUP;
                    break;
            }
            
            if (result < 0 && result != -EAGAIN && result != -EWOULDBLOCK) {
                result = -errno;
            }
        }
        
        io_result io_res{result, 0};
        
        ELIO_LOG_DEBUG("Completed io_op::{} on fd={}: result={}", 
                       static_cast<int>(op.req.op), op.req.fd, result);
        
        if (op.awaiter && !op.awaiter.done()) {
            if (deferred_resumes) {
                deferred_resumes->push_back({op.awaiter, io_res});
            } else {
                last_result_ = io_res;
                op.awaiter.resume();
            }
        }
    }
    
    /// Resume collected coroutine handles (call outside of lock)
    /// Sets last_result_ before resuming each coroutine
    static void resume_deferred(std::vector<deferred_resume_entry>& entries) {
        for (auto& entry : entries) {
            if (entry.handle && !entry.handle.done()) {
                last_result_ = entry.result;
                entry.handle.resume();
            }
        }
    }
    
private:
    int epoll_fd_ = -1;                                    ///< epoll file descriptor
    std::vector<struct epoll_event> events_;              ///< Event buffer for epoll_wait
    std::unordered_map<int, fd_state> fd_states_;         ///< Per-fd state
    timer_queue_t timer_queue_;                           ///< Timer queue for timeouts
    size_t pending_count_ = 0;                            ///< Number of pending operations
    int wake_fd_ = -1;  ///< eventfd for cross-thread wake-up

    static inline thread_local io_result last_result_{};
};

} // namespace elio::io
