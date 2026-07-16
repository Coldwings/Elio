#pragma once

#include "io_backend.hpp"
#include <elio/log/macros.hpp>
#include <elio/coro/vthread_stack.hpp>
#include <elio/coro/promise_base.hpp>
#include <elio/coro/frame.hpp>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
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
/// - Level-triggered mode (one op per fired event is safe; deregister-on-empty bounds wakeups)
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

        for (auto& op : ready_ops_) {
            enqueue_resume(op.req.state, op.awaiter,
                           io_result{-ECANCELED, 0}, &deferred_resumes);
        }
        ready_ops_.clear();
        
        for (auto& [fd, state] : fd_states_) {
            for (auto& op : state.pending_ops) {
                enqueue_resume(op.req.state, op.awaiter,
                               io_result{-ECANCELED, 0}, &deferred_resumes);
            }
        }
        fd_states_.clear();
        
        // Cancel all pending timers
        while (!timer_queue_.empty()) {
            auto& entry = timer_queue_.top();
            enqueue_resume(entry.state, entry.awaiter,
                           io_result{-ECANCELED, 0}, &deferred_resumes);
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
        
        // For epoll, we need to determine what events to watch for.
        // Use level-triggered mode: each completion executes exactly one syscall
        // and does not drain to EAGAIN, which would stall the next op under EPOLLET.
        // Spurious wakeups are bounded because we EPOLL_CTL_DEL the fd as soon as
        // its pending_ops queue becomes empty (see end of poll() and cancel()).
        uint32_t events = 0;
        
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
            case io_op::poll_write:
                events |= EPOLLOUT;
                break;

            case io_op::sendmsg:
                if (!req.msg) {
                    return false;
                }
                events |= EPOLLOUT;
                break;

            case io_op::connect: {
                socklen_t addrlen = req.addrlen ? *req.addrlen : 0;
                if (::connect(req.fd, req.addr, addrlen) == 0) {
                    return queue_ready_op(std::move(op), io_result{0, 0});
                }

                int err = errno;
                if (err == EISCONN) {
                    return queue_ready_op(std::move(op), io_result{0, 0});
                }
                if (err != EINPROGRESS && err != EALREADY &&
                    err != EAGAIN && err != EWOULDBLOCK) {
                    return queue_ready_op(std::move(op), io_result{-err, 0});
                }

                events |= EPOLLOUT;
                break;
            }
                
            case io_op::close:
                // Close operations are executed synchronously
                op.synchronous = true;
                break;
                
            case io_op::timeout: {
                // Use timer queue for timeout operations
                int64_t timeout_ns = static_cast<int64_t>(req.length);
                auto deadline = std::chrono::steady_clock::now() +
                                std::chrono::nanoseconds(timeout_ns);

                // cancel_key must match what the awaitable passes to
                // io_context::cancel(). For op_state-aware awaitables this
                // is tagged_op_state_user_data(req.state); for legacy
                // awaitables it is the raw coroutine handle address.
                void* ckey = req.state
                    ? tagged_op_state_user_data(req.state)
                    : req.awaiter.address();
                timer_queue_.push(timer_entry{deadline, req.awaiter, ckey, req.state});
                pending_count_++;

                ELIO_LOG_DEBUG("Prepared timeout: {}ns", timeout_ns);
                return true;
            }
                
            case io_op::cancel:
            case io_op::none:
                return false;
        }
        
        // Get or create fd state
        auto state_it = fd_states_.find(req.fd);
        if (state_it == fd_states_.end()) {
            state_it = fd_states_.emplace(req.fd, fd_state{}).first;
        }
        auto& state = state_it->second;
        bool is_sync = op.synchronous;
        
        if (!is_sync) {
            // Register with epoll
            uint32_t previous_events = state.events;
            bool previous_registered = state.registered;
            uint32_t requested_events = state.events | events;

            auto fail_registration = [&](int err) {
                state.events = previous_events;
                state.registered = previous_registered;
                io_result result{-err, 0};
                last_result_ = result;
                ELIO_LOG_WARNING("epoll_ctl failed for fd {}: {}",
                                 req.fd, strerror(err));
                if (state.pending_ops.empty() && !state.registered) {
                    fd_states_.erase(state_it);
                }
                return false;
            };
            
            struct epoll_event ev{};
            ev.events = requested_events;
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
            
            if (ret < 0) {
                int err = errno;
                if (err == EEXIST && !previous_registered) {
                    // fd is already registered (e.g. from a prior ADD that
                    // succeeded but whose state was lost). Apply the new
                    // interest mask before trusting the recovered state.
                    ret = epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, req.fd, &ev);
                    if (ret == 0) {
                        state.registered = true;
                    } else {
                        return fail_registration(errno);
                    }
                } else {
                    return fail_registration(err);
                }
            }

            state.events = requested_events;
        }
        
        state.pending_ops.push_back(std::move(op));
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

        // Collect handles to resume after processing all operations
        // (avoids iterator invalidation if resumed coroutine calls prepare())
        std::vector<deferred_resume_entry> deferred_resumes;

        // Execute synchronous operations (like close)
        for (auto& [fd, state] : fd_states_) {
            auto it = state.pending_ops.begin();
            while (it != state.pending_ops.end()) {
                if (it->synchronous) {
                    bool is_close = (it->req.op == io_op::close);
                    // Deregister from epoll BEFORE closing the fd to prevent
                    // a race where another thread reuses the fd number between
                    // close() and EPOLL_CTL_DEL, causing stale registrations.
                    if (is_close && state.registered) {
                        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                        state.registered = false;
                        state.events = 0;
                    }
                    execute_sync_op(*it, &deferred_resumes);
                    it = state.pending_ops.erase(it);
                    pending_count_--;
                    submitted++;
                } else {
                    ++it;
                }
            }
        }

        // Resume coroutines after iteration is complete
        resume_deferred(deferred_resumes);

        ELIO_LOG_DEBUG("Submitted {} synchronous operations", submitted);
        return submitted;
    }
    
    /// Poll for completed operations
    int poll(std::chrono::milliseconds timeout) override {
        int completions = 0;

        // Collect handles to resume after processing all completions
        std::vector<deferred_resume_entry> deferred_resumes;

        if (!ready_ops_.empty()) {
            auto ready_ops = std::move(ready_ops_);
            ready_ops_.clear();

            for (auto& op : ready_ops) {
                enqueue_resume(op.req.state, op.awaiter,
                               op.precompleted_result, &deferred_resumes);
                pending_count_--;
                completions++;
            }

            resume_deferred(deferred_resumes);
            ELIO_LOG_DEBUG("Processed {} ready completions", completions);
            return completions;
        }

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
                auto timer_timeout = std::chrono::ceil<std::chrono::milliseconds>(
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
        
        // Process expired timers
        auto now = std::chrono::steady_clock::now();
        while (!timer_queue_.empty() && timer_queue_.top().deadline <= now) {
            auto entry = timer_queue_.top();
            timer_queue_.pop();
            
            io_result result{0, 0};  // Timeout completed successfully
            
            enqueue_resume(entry.state, entry.awaiter, result, &deferred_resumes);
            
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

                // Process pending operations for this fd.
                // Level-triggered epoll will re-fire as long as the fd is
                // ready, so we consume at most ONE read-direction op and
                // ONE write-direction op per poll cycle. This prevents a
                // non-blocking syscall that returns EAGAIN from consuming
                // (and failing) every queued op of the same direction.
                bool consumed_read = false;
                bool consumed_write = false;
                auto op_it = state.pending_ops.begin();
                while (op_it != state.pending_ops.end()) {
                    bool is_read_dir = false;
                    bool is_write_dir = false;

                    switch (op_it->req.op) {
                        case io_op::read:
                        case io_op::readv:
                        case io_op::recv:
                        case io_op::accept:
                        case io_op::poll_read:
                            is_read_dir = true;
                            break;

                        case io_op::write:
                        case io_op::writev:
                        case io_op::sendmsg:
                        case io_op::send:
                        case io_op::connect:
                        case io_op::poll_write:
                            is_write_dir = true;
                            break;

                        default:
                            ++op_it;
                            continue;
                    }

                    bool ready = false;
                    if (is_read_dir && !consumed_read &&
                        (revents & (EPOLLIN | EPOLLHUP | EPOLLERR))) {
                        ready = true;
                    } else if (is_write_dir && !consumed_write &&
                               (revents & (EPOLLOUT | EPOLLHUP | EPOLLERR))) {
                        ready = true;
                    }

                    if (ready) {
                        execute_async_op(*op_it, revents, &deferred_resumes);
                        op_it = state.pending_ops.erase(op_it);
                        pending_count_--;
                        completions++;
                        if (is_read_dir) consumed_read = true;
                        if (is_write_dir) consumed_write = true;
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
        return pending_count_.load(std::memory_order_relaxed);
    }

    /// Cancel a pending operation
    bool cancel(void* user_data) override {
        deferred_resume_entry to_resume{};
        bool found_entry = false;

        auto ready_it = std::find_if(ready_ops_.begin(), ready_ops_.end(),
                                     [user_data](const pending_operation& op) {
                                         return cancel_key_for(op) == user_data;
                                     });
        if (ready_it != ready_ops_.end()) {
            found_entry = claim_resume(ready_it->req.state, ready_it->awaiter,
                                       io_result{-ECANCELED, 0}, to_resume);
            ready_ops_.erase(ready_it);
            pending_count_--;
            goto found;
        }
        
        // Search in fd_states first
        for (auto& [fd, state] : fd_states_) {
            auto it = std::find_if(state.pending_ops.begin(), 
                                    state.pending_ops.end(),
                                    [user_data](const pending_operation& op) {
                                        return cancel_key_for(op) == user_data;
                                    });
            
            if (it != state.pending_ops.end()) {
                found_entry = claim_resume(it->req.state, it->awaiter,
                                           io_result{-ECANCELED, 0}, to_resume);
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
                // Match against cancel_key (tagged op_state or awaiter address)
                if (entry.cancel_key == user_data) {
                    found_entry = claim_resume(entry.state, entry.awaiter,
                                               io_result{-ECANCELED, 0}, to_resume)
                                  || found_entry;
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
            safe_resume(to_resume.handle);
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
    struct pending_operation {
        io_request req;
        std::coroutine_handle<> awaiter;
        bool synchronous = false;
        io_result precompleted_result{0, 0};
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
        void* cancel_key = nullptr;  ///< Key for cancel matching (tagged op_state or awaiter address)
        op_state* state = nullptr;

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

    static bool claim_resume(op_state* state,
                             std::coroutine_handle<> fallback,
                             io_result result,
                             deferred_resume_entry& out) {
        auto handle = fallback;
        if (state) {
            state->result = result.result;
            state->flags = result.flags;
            handle = state->handle;

            uint8_t expected = op_state::phase_pending;
            if (!state->phase.compare_exchange_strong(
                    expected, op_state::phase_completed,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                if (expected == op_state::phase_orphaned) {
                    delete state;
                }
                return false;
            }
        }

        if (!handle || handle.done()) {
            return false;
        }
        out = {handle, result};
        return true;
    }

    static void* cancel_key_for(const pending_operation& op) noexcept {
        return op.req.state
            ? tagged_op_state_user_data(op.req.state)
            : op.awaiter.address();
    }

    bool queue_ready_op(pending_operation op, io_result result) {
        op.precompleted_result = result;
        ready_ops_.push_back(std::move(op));
        pending_count_++;
        notify();
        return true;
    }

    static void enqueue_resume(op_state* state,
                               std::coroutine_handle<> fallback,
                               io_result result,
                               std::vector<deferred_resume_entry>* deferred_resumes) {
        deferred_resume_entry entry{};
        if (!claim_resume(state, fallback, result, entry)) {
            return;
        }
        if (deferred_resumes) {
            deferred_resumes->push_back(entry);
        } else {
            last_result_ = entry.result;
            safe_resume(entry.handle);
        }
    }
    
    /// Min-heap priority queue for timers
    using timer_queue_t = std::priority_queue<timer_entry, 
                                               std::vector<timer_entry>,
                                               std::greater<timer_entry>>;
    
    void execute_sync_op(pending_operation& op,
                         std::vector<deferred_resume_entry>* deferred_resumes = nullptr) {
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

        io_result res{result, 0};

        enqueue_resume(op.req.state, op.awaiter, res, deferred_resumes);
    }
    
    /// Execute async I/O operation
    /// @param op The pending operation to execute
    /// @param revents The epoll events that triggered this operation
    /// @param deferred_resumes If non-null, collect handle+result for later resumption (avoids deadlock)
    void execute_async_op(pending_operation& op, uint32_t revents,
                          std::vector<deferred_resume_entry>* deferred_resumes = nullptr) {
        int result = 0;
        bool syscall_result = false;
        
        auto is_read_data_op = [](io_op op) noexcept {
            switch (op) {
                case io_op::read:
                case io_op::readv:
                case io_op::recv:
                    return true;
                default:
                    return false;
            }
        };

        // Check for errors first
        if (revents & EPOLLERR) {
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(op.req.fd, SOL_SOCKET, SO_ERROR, &error, &len);
            result = -error;
        } else if ((revents & EPOLLHUP) && !is_read_data_op(op.req.op)) {
            // Non-read data operations cannot drain bytes on HUP.
            result = -EPIPE;
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
                    syscall_result = true;
                    break;
                    
                case io_op::write:
                    if (op.req.offset >= 0) {
                        result = static_cast<int>(pwrite(op.req.fd, op.req.buffer, 
                                                         op.req.length, op.req.offset));
                    } else {
                        result = static_cast<int>(write(op.req.fd, op.req.buffer, 
                                                        op.req.length));
                    }
                    syscall_result = true;
                    break;
                    
                case io_op::readv:
                    result = static_cast<int>(readv(op.req.fd, op.req.iovecs, 
                                                    static_cast<int>(op.req.iovec_count)));
                    syscall_result = true;
                    break;
                    
                case io_op::writev:
                    result = static_cast<int>(writev(op.req.fd, op.req.iovecs, 
                                                     static_cast<int>(op.req.iovec_count)));
                    syscall_result = true;
                    break;

                case io_op::sendmsg:
                    result = static_cast<int>(sendmsg(op.req.fd, op.req.msg,
                                                      op.req.socket_flags));
                    syscall_result = true;
                    break;
                    
                case io_op::recv:
                    result = static_cast<int>(recv(op.req.fd, op.req.buffer, 
                                                   op.req.length, op.req.socket_flags));
                    syscall_result = true;
                    break;
                    
                case io_op::send:
                    result = static_cast<int>(send(op.req.fd, op.req.buffer, 
                                                   op.req.length, op.req.socket_flags));
                    syscall_result = true;
                    break;
                    
                case io_op::accept:
                    result = accept4(op.req.fd, op.req.addr, op.req.addrlen, 
                                     op.req.socket_flags | SOCK_CLOEXEC);
                    syscall_result = true;
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
            
            if (syscall_result && result < 0) {
                result = -errno;
            }
        }
        
        io_result io_res{result, 0};
        
        ELIO_LOG_DEBUG("Completed io_op::{} on fd={}: result={}", 
                       static_cast<int>(op.req.op), op.req.fd, result);
        
        enqueue_resume(op.req.state, op.awaiter, io_res, deferred_resumes);
    }

    /// Safely resume a coroutine handle with proper coroutine context.
    /// This mirrors scheduler worker resumes so both the vthread stack and
    /// virtual frame chain are valid while user code runs after I/O completion.
    static void safe_resume(std::coroutine_handle<> handle) {
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
    }

    /// Resume collected coroutine handles (call outside of lock)
    /// Sets last_result_ before resuming each coroutine
    static void resume_deferred(std::vector<deferred_resume_entry>& entries) {
        for (auto& entry : entries) {
            if (entry.handle && !entry.handle.done()) {
                last_result_ = entry.result;
                safe_resume(entry.handle);
            }
        }
    }
    
private:
    int epoll_fd_ = -1;                                    ///< epoll file descriptor
    std::vector<struct epoll_event> events_;              ///< Event buffer for epoll_wait
    std::unordered_map<int, fd_state> fd_states_;         ///< Per-fd state
    std::vector<pending_operation> ready_ops_;            ///< Ops completed during prepare()
    timer_queue_t timer_queue_;                           ///< Timer queue for timeouts
    std::atomic<size_t> pending_count_{0};                ///< Number of pending operations (atomic for cross-thread reads)
    int wake_fd_ = -1;  ///< eventfd for cross-thread wake-up

    static inline thread_local io_result last_result_{};
};

} // namespace elio::io
