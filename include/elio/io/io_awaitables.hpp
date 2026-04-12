#pragma once

#include "io_context.hpp"
#include <elio/log/macros.hpp>
#include <elio/coro/promise_base.hpp>
#include <elio/coro/frame.hpp>
#include <elio/runtime/worker_thread.hpp>
#include <coroutine>
#include <span>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>

namespace elio::io {

/// Get the io_context for the current worker thread
/// Falls back to default_io_context if not running in a worker
inline io_context& current_io_context() noexcept {
    auto* worker = runtime::worker_thread::current();
    if (worker) {
        return worker->io_context();
    }
    return default_io_context();
}

/// Base class for I/O awaitables
/// Provides common functionality for all async I/O operations
class io_awaitable_base {
public:
    io_awaitable_base() noexcept = default;
    
    /// Never ready immediately - always suspend
    bool await_ready() const noexcept {
        return false;
    }
    
    /// Get the result of the I/O operation
    io_result result() const noexcept {
        return result_;
    }
    
protected:
    io_result result_{};
    size_t saved_affinity_ = coro::NO_AFFINITY;
    void* handle_address_ = nullptr;
    
    /// Save current affinity and bind to current worker
    template<typename Promise>
    void bind_to_worker(std::coroutine_handle<Promise> handle) {
        handle_address_ = handle.address();
        if constexpr (std::is_base_of_v<coro::promise_base, Promise>) {
            saved_affinity_ = handle.promise().affinity();
            auto* worker = runtime::worker_thread::current();
            if (worker) {
                handle.promise().set_affinity(worker->worker_id());
            }
        }
    }
    
    /// Restore previous affinity (called from await_resume)
    void restore_affinity() {
        if (handle_address_) {
            auto* promise = coro::get_promise_base(handle_address_);
            if (promise) {
                if (saved_affinity_ == coro::NO_AFFINITY) {
                    promise->clear_affinity();
                } else {
                    promise->set_affinity(saved_affinity_);
                }
            }
        }
    }
};

/// Awaitable for async read operations
class async_read_awaitable : public io_awaitable_base {
public:
    async_read_awaitable(int fd, void* buffer, size_t length, 
                         int64_t offset = -1) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , buffer_(buffer)
        , length_(length)
        , offset_(offset) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();
        
        io_request req{};
        req.op = io_op::read;
        req.fd = fd_;
        req.buffer = buffer_;
        req.length = length_;
        req.offset = offset_;
        req.awaiter = awaiter;
        
        if (!ctx.prepare(req)) {
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx.submit();
    }
    
    io_result await_resume() noexcept {
        result_ = io_context::get_last_result();
        restore_affinity();
        return result_;
    }
    
private:
    int fd_;
    void* buffer_;
    size_t length_;
    int64_t offset_;
};

/// Awaitable for async write operations
class async_write_awaitable : public io_awaitable_base {
public:
    async_write_awaitable(int fd, const void* buffer, 
                          size_t length, int64_t offset = -1) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , buffer_(buffer)
        , length_(length)
        , offset_(offset) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();
        
        io_request req{};
        req.op = io_op::write;
        req.fd = fd_;
        req.buffer = const_cast<void*>(buffer_);
        req.length = length_;
        req.offset = offset_;
        req.awaiter = awaiter;
        
        if (!ctx.prepare(req)) {
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx.submit();
    }
    
    io_result await_resume() noexcept {
        result_ = io_context::get_last_result();
        restore_affinity();
        return result_;
    }
    
private:
    int fd_;
    const void* buffer_;
    size_t length_;
    int64_t offset_;
};

/// Awaitable for async recv operations
class async_recv_awaitable : public io_awaitable_base {
public:
    async_recv_awaitable(int fd, void* buffer, size_t length,
                         int flags = 0) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , buffer_(buffer)
        , length_(length)
        , flags_(flags) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();
        
        io_request req{};
        req.op = io_op::recv;
        req.fd = fd_;
        req.buffer = buffer_;
        req.length = length_;
        req.socket_flags = flags_;
        req.awaiter = awaiter;
        
        if (!ctx.prepare(req)) {
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx.submit();
    }
    
    io_result await_resume() noexcept {
        result_ = io_context::get_last_result();
        restore_affinity();
        return result_;
    }
    
private:
    int fd_;
    void* buffer_;
    size_t length_;
    int flags_;
};

/// Awaitable for async send operations
class async_send_awaitable : public io_awaitable_base {
public:
    async_send_awaitable(int fd, const void* buffer, 
                         size_t length, int flags = 0) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , buffer_(buffer)
        , length_(length)
        , flags_(flags) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();
        
        io_request req{};
        req.op = io_op::send;
        req.fd = fd_;
        req.buffer = const_cast<void*>(buffer_);
        req.length = length_;
        req.socket_flags = flags_;
        req.awaiter = awaiter;
        
        if (!ctx.prepare(req)) {
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx.submit();
    }
    
    io_result await_resume() noexcept {
        result_ = io_context::get_last_result();
        restore_affinity();
        return result_;
    }
    
private:
    int fd_;
    const void* buffer_;
    size_t length_;
    int flags_;
};

/// Awaitable for async accept operations
class async_accept_awaitable : public io_awaitable_base {
public:
    async_accept_awaitable(int listen_fd, 
                           struct sockaddr* addr = nullptr,
                           socklen_t* addrlen = nullptr,
                           int flags = 0) noexcept
        : io_awaitable_base()
        , listen_fd_(listen_fd)
        , addr_(addr)
        , addrlen_(addrlen)
        , flags_(flags) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();
        
        io_request req{};
        req.op = io_op::accept;
        req.fd = listen_fd_;
        req.addr = addr_;
        req.addrlen = addrlen_;
        req.socket_flags = flags_;
        req.awaiter = awaiter;
        
        if (!ctx.prepare(req)) {
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx.submit();
    }
    
    io_result await_resume() noexcept {
        result_ = io_context::get_last_result();
        restore_affinity();
        return result_;
    }
    
    /// Convenience: get the accepted fd directly
    int accepted_fd() const noexcept {
        return result_.success() ? result_.result : -1;
    }
    
private:
    int listen_fd_;
    struct sockaddr* addr_;
    socklen_t* addrlen_;
    int flags_;
};

/// Awaitable for async connect operations
class async_connect_awaitable : public io_awaitable_base {
public:
    async_connect_awaitable(int fd, 
                            const struct sockaddr* addr,
                            socklen_t addrlen) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , addr_(addr)
        , addrlen_(addrlen) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();
        
        io_request req{};
        req.op = io_op::connect;
        req.fd = fd_;
        req.addr = const_cast<struct sockaddr*>(addr_);
        req.addrlen = &addrlen_;
        req.awaiter = awaiter;
        
        if (!ctx.prepare(req)) {
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx.submit();
    }
    
    io_result await_resume() noexcept {
        result_ = io_context::get_last_result();
        restore_affinity();
        return result_;
    }
    
private:
    int fd_;
    const struct sockaddr* addr_;
    socklen_t addrlen_;
};

/// Awaitable for async close operations
class async_close_awaitable : public io_awaitable_base {
public:
    async_close_awaitable(int fd) noexcept
        : io_awaitable_base()
        , fd_(fd) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();
        
        io_request req{};
        req.op = io_op::close;
        req.fd = fd_;
        req.awaiter = awaiter;
        
        if (!ctx.prepare(req)) {
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx.submit();
    }
    
    io_result await_resume() noexcept {
        result_ = io_context::get_last_result();
        restore_affinity();
        return result_;
    }
    
private:
    int fd_;
};

/// Awaitable for async writev operations (scatter-gather write)
class async_writev_awaitable : public io_awaitable_base {
public:
    async_writev_awaitable(int fd, struct iovec* iovecs,
                           size_t iovec_count) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , iovecs_(iovecs)
        , iovec_count_(iovec_count) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();
        
        io_request req{};
        req.op = io_op::writev;
        req.fd = fd_;
        req.iovecs = iovecs_;
        req.iovec_count = iovec_count_;
        req.awaiter = awaiter;
        
        if (!ctx.prepare(req)) {
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx.submit();
    }
    
    io_result await_resume() noexcept {
        result_ = io_context::get_last_result();
        restore_affinity();
        return result_;
    }
    
private:
    int fd_;
    struct iovec* iovecs_;
    size_t iovec_count_;
};

/// Awaitable for poll (wait for socket readable/writable)
class async_poll_awaitable : public io_awaitable_base {
public:
    async_poll_awaitable(int fd, bool for_read) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , for_read_(for_read) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();
        
        io_request req{};
        req.op = for_read_ ? io_op::poll_read : io_op::poll_write;
        req.fd = fd_;
        req.awaiter = awaiter;
        
        if (!ctx.prepare(req)) {
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx.submit();
    }
    
    io_result await_resume() noexcept {
        result_ = io_context::get_last_result();
        restore_affinity();
        return result_;
    }
    
private:
    int fd_;
    bool for_read_;
};

/// Factory functions for creating awaitables

/// Create an async read awaitable
/// @param fd File descriptor to read from
/// @param buffer Buffer to read into
/// @param length Number of bytes to read
/// @param offset File offset (-1 for current position)
inline auto async_read(int fd, void* buffer, size_t length, 
                       int64_t offset = -1) {
    return async_read_awaitable(fd, buffer, length, offset);
}

/// Create an async read awaitable using span
template<typename T>
inline auto async_read(int fd, std::span<T> buffer, 
                       int64_t offset = -1) {
    return async_read_awaitable(fd, buffer.data(), 
                                buffer.size_bytes(), offset);
}

/// Create an async write awaitable
inline auto async_write(int fd, const void* buffer, 
                        size_t length, int64_t offset = -1) {
    return async_write_awaitable(fd, buffer, length, offset);
}

/// Create an async write awaitable using span
template<typename T>
inline auto async_write(int fd, std::span<const T> buffer,
                        int64_t offset = -1) {
    return async_write_awaitable(fd, buffer.data(), 
                                 buffer.size_bytes(), offset);
}

/// Create an async recv awaitable
inline auto async_recv(int fd, void* buffer, size_t length,
                       int flags = 0) {
    return async_recv_awaitable(fd, buffer, length, flags);
}

/// Create an async send awaitable
inline auto async_send(int fd, const void* buffer, 
                       size_t length, int flags = 0) {
    return async_send_awaitable(fd, buffer, length, flags);
}

/// Create an async writev awaitable (scatter-gather write)
inline auto async_writev(int fd, struct iovec* iovecs,
                         size_t iovec_count) {
    return async_writev_awaitable(fd, iovecs, iovec_count);
}

/// Create an async accept awaitable
inline auto async_accept(int listen_fd,
                         struct sockaddr* addr = nullptr,
                         socklen_t* addrlen = nullptr,
                         int flags = 0) {
    return async_accept_awaitable(listen_fd, addr, addrlen, flags);
}

/// Create an async connect awaitable
inline auto async_connect(int fd, 
                          const struct sockaddr* addr, socklen_t addrlen) {
    return async_connect_awaitable(fd, addr, addrlen);
}

/// Create an async close awaitable
inline auto async_close(int fd) {
    return async_close_awaitable(fd);
}

/// Create an async poll awaitable for reading
inline auto async_poll_read(int fd) {
    return async_poll_awaitable(fd, true);
}

/// Create an async poll awaitable for writing
inline auto async_poll_write(int fd) {
    return async_poll_awaitable(fd, false);
}

/// Segment descriptor for batch read operations
struct batch_read_segment {
    int64_t offset;   ///< File offset (-1 for current position → uses pread with offset 0)
    void* buffer;     ///< Destination buffer
    size_t length;    ///< Bytes to read
};

/// Segment descriptor for batch write operations
struct batch_write_segment {
    int64_t offset;      ///< File offset (-1 for current position → uses pwrite with offset 0)
    const void* buffer;  ///< Source data
    size_t length;       ///< Bytes to write
};

/// Tracks completion of a batch I/O operation
struct batch_state {
    std::atomic<int> completed{0}; ///< Atomically incremented per completion
    int total;                      ///< Total segments
    std::vector<int> results;       ///< Per-segment results (bytes or -errno)

    explicit batch_state(int n) : total(n), results(n) {}

    bool all_done() const noexcept {
        return completed.load(std::memory_order_acquire) >= total;
    }
};

/// Awaitable for batch read operations
/// Submits multiple pread operations in a single io_uring syscall.
/// Falls back to sequential synchronous reads for epoll backend.
class batch_read_awaitable : public io_awaitable_base {
public:
    batch_read_awaitable(int fd, std::span<const batch_read_segment> segments) noexcept
        : io_awaitable_base(), fd_(fd), segments_(segments.begin(), segments.end()) {}

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);

        if (segments_.empty()) {
            batch_st_ = std::make_unique<batch_state>(0);
            awaiter.resume();
            return;
        }

        auto& ctx = current_io_context();

#if ELIO_HAS_IO_URING
        auto* backend = dynamic_cast<io_uring_backend*>(ctx.get_backend());
        if (backend && backend->get_ring()) {
            struct io_uring* ring = backend->get_ring();
            batch_st_ = std::make_unique<batch_state>(static_cast<int>(segments_.size()));

            // Prepare all SQEs at once, using a unique tag per segment
            for (size_t i = 0; i < segments_.size(); ++i) {
                struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
                if (!sqe) {
                    batch_st_->results[i] = -EAGAIN;
                    batch_st_->completed.fetch_add(1, std::memory_order_acq_rel);
                    continue;
                }
                // Store segment index + batch pointer in user_data
                // Use a simple scheme: encode index in lower bits of a combined value
                auto* tagged = reinterpret_cast<void*>(
                    (reinterpret_cast<uintptr_t>(batch_st_.get()) << 8) |
                    (i & 0x7F) | 0x80);
                io_uring_sqe_set_data(sqe, tagged);
                auto off = static_cast<__u64>(
                    segments_[i].offset >= 0 ? segments_[i].offset : 0);
                io_uring_prep_read(sqe, fd_, segments_[i].buffer,
                    static_cast<unsigned>(segments_[i].length), off);
            }

            // Single syscall submit
            io_uring_submit(ring);

            // Poll until all segments complete
            while (!batch_st_->all_done()) {
                struct io_uring_cqe* wcqe = nullptr;
                int ret = io_uring_wait_cqe(ring, &wcqe);
                if (ret != 0 || !wcqe) continue;
                auto* ud = io_uring_cqe_get_data(wcqe);
                auto val = reinterpret_cast<uintptr_t>(ud);
                if (val & 0x80) {
                    auto* st = reinterpret_cast<batch_state*>(val >> 8);
                    if (st == batch_st_.get()) {
                        int seg_idx = static_cast<int>(val & 0x7F);
                        if (seg_idx < static_cast<int>(st->results.size())) {
                            st->results[seg_idx] = wcqe->res;
                        }
                        st->completed.fetch_add(1, std::memory_order_acq_rel);
                    }
                }
                io_uring_cqe_seen(ring, wcqe);
            }
            awaiter.resume();
            return;
        }
#endif
        // Fallback: sequential synchronous reads
        batch_st_ = std::make_unique<batch_state>(static_cast<int>(segments_.size()));
        for (size_t i = 0; i < segments_.size(); ++i) {
            auto off = segments_[i].offset >= 0 ? segments_[i].offset : 0;
            batch_st_->results[i] = static_cast<int>(
                pread(fd_, segments_[i].buffer, segments_[i].length, off));
        }
        awaiter.resume();
    }

    std::vector<int> await_resume() noexcept {
        restore_affinity();
        if (batch_st_) {
            return std::move(batch_st_->results);
        }
        return {};
    }

private:
    int fd_;
    std::vector<batch_read_segment> segments_;
    std::unique_ptr<batch_state> batch_st_;
};

/// Awaitable for batch write operations
/// Submits multiple pwrite operations in a single io_uring syscall.
/// Falls back to sequential synchronous writes for epoll backend.
class batch_write_awaitable : public io_awaitable_base {
public:
    batch_write_awaitable(int fd, std::span<const batch_write_segment> segments) noexcept
        : io_awaitable_base(), fd_(fd), segments_(segments.begin(), segments.end()) {}

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);

        if (segments_.empty()) {
            batch_st_ = std::make_unique<batch_state>(0);
            awaiter.resume();
            return;
        }

        auto& ctx = current_io_context();

#if ELIO_HAS_IO_URING
        auto* backend = dynamic_cast<io_uring_backend*>(ctx.get_backend());
        if (backend && backend->get_ring()) {
            struct io_uring* ring = backend->get_ring();
            batch_st_ = std::make_unique<batch_state>(static_cast<int>(segments_.size()));

            for (size_t i = 0; i < segments_.size(); ++i) {
                struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
                if (!sqe) {
                    batch_st_->results[i] = -EAGAIN;
                    batch_st_->completed.fetch_add(1, std::memory_order_acq_rel);
                    continue;
                }
                auto* tagged = reinterpret_cast<void*>(
                    (reinterpret_cast<uintptr_t>(batch_st_.get()) << 8) |
                    (i & 0x7F) | 0x80);
                io_uring_sqe_set_data(sqe, tagged);
                auto off = static_cast<__u64>(
                    segments_[i].offset >= 0 ? segments_[i].offset : 0);
                io_uring_prep_write(sqe, fd_, segments_[i].buffer,
                    static_cast<unsigned>(segments_[i].length), off);
            }

            io_uring_submit(ring);

            while (!batch_st_->all_done()) {
                struct io_uring_cqe* wcqe = nullptr;
                int ret = io_uring_wait_cqe(ring, &wcqe);
                if (ret != 0 || !wcqe) continue;
                auto* ud = io_uring_cqe_get_data(wcqe);
                auto val = reinterpret_cast<uintptr_t>(ud);
                if (val & 0x80) {
                    auto* st = reinterpret_cast<batch_state*>(val >> 8);
                    if (st == batch_st_.get()) {
                        int seg_idx = static_cast<int>(val & 0x7F);
                        if (seg_idx < static_cast<int>(st->results.size())) {
                            st->results[seg_idx] = wcqe->res;
                        }
                        st->completed.fetch_add(1, std::memory_order_acq_rel);
                    }
                }
                io_uring_cqe_seen(ring, wcqe);
            }
            awaiter.resume();
            return;
        }
#endif
        // Fallback: sequential synchronous writes
        batch_st_ = std::make_unique<batch_state>(static_cast<int>(segments_.size()));
        for (size_t i = 0; i < segments_.size(); ++i) {
            auto off = segments_[i].offset >= 0 ? segments_[i].offset : 0;
            batch_st_->results[i] = static_cast<int>(
                pwrite(fd_, segments_[i].buffer, segments_[i].length, off));
        }
        awaiter.resume();
    }

    std::vector<int> await_resume() noexcept {
        restore_affinity();
        if (batch_st_) {
            return std::move(batch_st_->results);
        }
        return {};
    }

private:
    int fd_;
    std::vector<batch_write_segment> segments_;
    std::unique_ptr<batch_state> batch_st_;
};

/// Batch read from file at multiple offsets in a single syscall (io_uring)
inline auto batch_read(int fd, std::span<const batch_read_segment> segments) {
    return batch_read_awaitable(fd, segments);
}

/// Batch write to file at multiple offsets in a single syscall (io_uring)
inline auto batch_write(int fd, std::span<const batch_write_segment> segments) {
    return batch_write_awaitable(fd, segments);
}

} // namespace elio::io
