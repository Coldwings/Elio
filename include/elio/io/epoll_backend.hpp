#pragma once

#include "io_backend.hpp"
#include <elio/log/macros.hpp>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <queue>
#include <mutex>

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
    }
    
    /// Destructor
    ~epoll_backend() override {
        // Collect handles to resume after releasing mutex
        std::vector<std::coroutine_handle<>> deferred_resumes;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [fd, state] : fd_states_) {
                for (auto& op : state.pending_ops) {
                    if (op.awaiter && !op.awaiter.done()) {
                        last_result_ = io_result{-ECANCELED, 0};
                        deferred_resumes.push_back(op.awaiter);
                    }
                }
            }
            fd_states_.clear();
        } // mutex released here
        
        // Resume outside lock to prevent deadlock
        resume_deferred(deferred_resumes);
        
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
        std::lock_guard<std::mutex> lock(mutex_);
        
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
                
            case io_op::timeout:
                // Timeouts are handled separately
                op.is_timeout = true;
                op.timeout_ns = static_cast<int64_t>(req.length);
                break;
                
            case io_op::cancel:
            case io_op::none:
                return false;
        }
        
        // Get or create fd state
        auto& state = fd_states_[req.fd];
        bool is_sync = op.synchronous;
        bool is_timo = op.is_timeout;
        state.pending_ops.push_back(std::move(op));
        
        if (!is_sync && !is_timo) {
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
        std::lock_guard<std::mutex> lock(mutex_);
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
        
        // Collect handles to resume after releasing mutex (prevents deadlock
        // when resumed coroutines call prepare())
        std::vector<std::coroutine_handle<>> deferred_resumes;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            
            for (int i = 0; i < nfds; ++i) {
                int fd = events_[i].data.fd;
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
        } // mutex released here
        
        // Resume coroutines outside the lock to prevent deadlock
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
        std::coroutine_handle<> to_resume;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            
            for (auto& [fd, state] : fd_states_) {
                auto it = std::find_if(state.pending_ops.begin(), 
                                        state.pending_ops.end(),
                                        [user_data](const pending_operation& op) {
                                            return op.awaiter.address() == user_data;
                                        });
                
                if (it != state.pending_ops.end()) {
                    // Collect handle for deferred resumption
                    last_result_ = io_result{-ECANCELED, 0};
                    if (it->awaiter && !it->awaiter.done()) {
                        to_resume = it->awaiter;
                    }
                    state.pending_ops.erase(it);
                    pending_count_--;
                    break;
                }
            }
        } // mutex released here
        
        // Resume outside lock to prevent deadlock
        if (to_resume && !to_resume.done()) {
            to_resume.resume();
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
    
private:
    struct pending_operation {
        io_request req;
        std::coroutine_handle<> awaiter;
        bool synchronous = false;
        bool is_timeout = false;
        int64_t timeout_ns = 0;
    };
    
    struct fd_state {
        std::vector<pending_operation> pending_ops;
        uint32_t events = 0;
        bool registered = false;
    };
    
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
    /// @param deferred_resumes If non-null, collect handle for later resumption (avoids deadlock)
    void execute_async_op(pending_operation& op, uint32_t revents,
                          std::vector<std::coroutine_handle<>>* deferred_resumes = nullptr) {
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
        
        last_result_ = io_result{result, 0};
        
        ELIO_LOG_DEBUG("Completed io_op::{} on fd={}: result={}", 
                       static_cast<int>(op.req.op), op.req.fd, result);
        
        if (op.awaiter && !op.awaiter.done()) {
            if (deferred_resumes) {
                deferred_resumes->push_back(op.awaiter);
            } else {
                op.awaiter.resume();
            }
        }
    }
    
    /// Resume collected coroutine handles (call outside of lock)
    static void resume_deferred(std::vector<std::coroutine_handle<>>& handles) {
        for (auto& h : handles) {
            if (h && !h.done()) {
                h.resume();
            }
        }
    }
    
private:
    int epoll_fd_ = -1;                                    ///< epoll file descriptor
    std::vector<struct epoll_event> events_;              ///< Event buffer for epoll_wait
    std::unordered_map<int, fd_state> fd_states_;         ///< Per-fd state
    size_t pending_count_ = 0;                            ///< Number of pending operations
    mutable std::mutex mutex_;                            ///< Protects fd_states_
    
    static inline thread_local io_result last_result_{};
};

} // namespace elio::io
