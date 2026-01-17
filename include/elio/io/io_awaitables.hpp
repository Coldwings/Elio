#pragma once

#include "io_context.hpp"
#include <elio/log/macros.hpp>
#include <coroutine>
#include <span>
#include <sys/socket.h>
#include <netinet/in.h>

namespace elio::io {

/// Base class for I/O awaitables
/// Provides common functionality for all async I/O operations
class io_awaitable_base {
public:
    explicit io_awaitable_base(io_context& ctx) noexcept 
        : ctx_(ctx) {}
    
    /// Never ready immediately - always suspend
    bool await_ready() const noexcept {
        return false;
    }
    
    /// Get the result of the I/O operation
    io_result result() const noexcept {
        return result_;
    }
    
protected:
    io_context& ctx_;
    io_result result_{};
};

/// Awaitable for async read operations
class async_read_awaitable : public io_awaitable_base {
public:
    async_read_awaitable(io_context& ctx, int fd, void* buffer, size_t length, 
                         int64_t offset = -1) noexcept
        : io_awaitable_base(ctx)
        , fd_(fd)
        , buffer_(buffer)
        , length_(length)
        , offset_(offset) {}
    
    void await_suspend(std::coroutine_handle<> awaiter) {
        io_request req{};
        req.op = io_op::read;
        req.fd = fd_;
        req.buffer = buffer_;
        req.length = length_;
        req.offset = offset_;
        req.awaiter = awaiter;
        
        if (!ctx_.prepare(req)) {
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx_.submit();
    }
    
    io_result await_resume() noexcept {
        result_ = io_context::get_last_result();
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
    async_write_awaitable(io_context& ctx, int fd, const void* buffer, 
                          size_t length, int64_t offset = -1) noexcept
        : io_awaitable_base(ctx)
        , fd_(fd)
        , buffer_(buffer)
        , length_(length)
        , offset_(offset) {}
    
    void await_suspend(std::coroutine_handle<> awaiter) {
        io_request req{};
        req.op = io_op::write;
        req.fd = fd_;
        req.buffer = const_cast<void*>(buffer_);
        req.length = length_;
        req.offset = offset_;
        req.awaiter = awaiter;
        
        if (!ctx_.prepare(req)) {
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx_.submit();
    }
    
    io_result await_resume() noexcept {
        result_ = io_context::get_last_result();
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
    async_recv_awaitable(io_context& ctx, int fd, void* buffer, size_t length,
                         int flags = 0) noexcept
        : io_awaitable_base(ctx)
        , fd_(fd)
        , buffer_(buffer)
        , length_(length)
        , flags_(flags) {}
    
    void await_suspend(std::coroutine_handle<> awaiter) {
        io_request req{};
        req.op = io_op::recv;
        req.fd = fd_;
        req.buffer = buffer_;
        req.length = length_;
        req.socket_flags = flags_;
        req.awaiter = awaiter;
        
        if (!ctx_.prepare(req)) {
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx_.submit();
    }
    
    io_result await_resume() noexcept {
        result_ = io_context::get_last_result();
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
    async_send_awaitable(io_context& ctx, int fd, const void* buffer, 
                         size_t length, int flags = 0) noexcept
        : io_awaitable_base(ctx)
        , fd_(fd)
        , buffer_(buffer)
        , length_(length)
        , flags_(flags) {}
    
    void await_suspend(std::coroutine_handle<> awaiter) {
        io_request req{};
        req.op = io_op::send;
        req.fd = fd_;
        req.buffer = const_cast<void*>(buffer_);
        req.length = length_;
        req.socket_flags = flags_;
        req.awaiter = awaiter;
        
        if (!ctx_.prepare(req)) {
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx_.submit();
    }
    
    io_result await_resume() noexcept {
        result_ = io_context::get_last_result();
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
    async_accept_awaitable(io_context& ctx, int listen_fd, 
                           struct sockaddr* addr = nullptr,
                           socklen_t* addrlen = nullptr,
                           int flags = 0) noexcept
        : io_awaitable_base(ctx)
        , listen_fd_(listen_fd)
        , addr_(addr)
        , addrlen_(addrlen)
        , flags_(flags) {}
    
    void await_suspend(std::coroutine_handle<> awaiter) {
        io_request req{};
        req.op = io_op::accept;
        req.fd = listen_fd_;
        req.addr = addr_;
        req.addrlen = addrlen_;
        req.socket_flags = flags_;
        req.awaiter = awaiter;
        
        if (!ctx_.prepare(req)) {
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx_.submit();
    }
    
    io_result await_resume() noexcept {
        result_ = io_context::get_last_result();
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
    async_connect_awaitable(io_context& ctx, int fd, 
                            const struct sockaddr* addr,
                            socklen_t addrlen) noexcept
        : io_awaitable_base(ctx)
        , fd_(fd)
        , addr_(addr)
        , addrlen_(addrlen) {}
    
    void await_suspend(std::coroutine_handle<> awaiter) {
        io_request req{};
        req.op = io_op::connect;
        req.fd = fd_;
        req.addr = const_cast<struct sockaddr*>(addr_);
        req.addrlen = &addrlen_;
        req.awaiter = awaiter;
        
        if (!ctx_.prepare(req)) {
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx_.submit();
    }
    
    io_result await_resume() noexcept {
        result_ = io_context::get_last_result();
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
    async_close_awaitable(io_context& ctx, int fd) noexcept
        : io_awaitable_base(ctx)
        , fd_(fd) {}
    
    void await_suspend(std::coroutine_handle<> awaiter) {
        io_request req{};
        req.op = io_op::close;
        req.fd = fd_;
        req.awaiter = awaiter;
        
        if (!ctx_.prepare(req)) {
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx_.submit();
    }
    
    io_result await_resume() noexcept {
        result_ = io_context::get_last_result();
        return result_;
    }
    
private:
    int fd_;
};

/// Awaitable for poll (wait for socket readable/writable)
class async_poll_awaitable : public io_awaitable_base {
public:
    async_poll_awaitable(io_context& ctx, int fd, bool for_read) noexcept
        : io_awaitable_base(ctx)
        , fd_(fd)
        , for_read_(for_read) {}
    
    void await_suspend(std::coroutine_handle<> awaiter) {
        io_request req{};
        req.op = for_read_ ? io_op::poll_read : io_op::poll_write;
        req.fd = fd_;
        req.awaiter = awaiter;
        
        if (!ctx_.prepare(req)) {
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx_.submit();
    }
    
    io_result await_resume() noexcept {
        result_ = io_context::get_last_result();
        return result_;
    }
    
private:
    int fd_;
    bool for_read_;
};

/// Factory functions for creating awaitables

/// Create an async read awaitable
/// @param ctx The I/O context
/// @param fd File descriptor to read from
/// @param buffer Buffer to read into
/// @param length Number of bytes to read
/// @param offset File offset (-1 for current position)
inline auto async_read(io_context& ctx, int fd, void* buffer, size_t length, 
                       int64_t offset = -1) {
    return async_read_awaitable(ctx, fd, buffer, length, offset);
}

/// Create an async read awaitable using span
template<typename T>
inline auto async_read(io_context& ctx, int fd, std::span<T> buffer, 
                       int64_t offset = -1) {
    return async_read_awaitable(ctx, fd, buffer.data(), 
                                buffer.size_bytes(), offset);
}

/// Create an async write awaitable
inline auto async_write(io_context& ctx, int fd, const void* buffer, 
                        size_t length, int64_t offset = -1) {
    return async_write_awaitable(ctx, fd, buffer, length, offset);
}

/// Create an async write awaitable using span
template<typename T>
inline auto async_write(io_context& ctx, int fd, std::span<const T> buffer,
                        int64_t offset = -1) {
    return async_write_awaitable(ctx, fd, buffer.data(), 
                                 buffer.size_bytes(), offset);
}

/// Create an async recv awaitable
inline auto async_recv(io_context& ctx, int fd, void* buffer, size_t length,
                       int flags = 0) {
    return async_recv_awaitable(ctx, fd, buffer, length, flags);
}

/// Create an async send awaitable
inline auto async_send(io_context& ctx, int fd, const void* buffer, 
                       size_t length, int flags = 0) {
    return async_send_awaitable(ctx, fd, buffer, length, flags);
}

/// Create an async accept awaitable
inline auto async_accept(io_context& ctx, int listen_fd,
                         struct sockaddr* addr = nullptr,
                         socklen_t* addrlen = nullptr,
                         int flags = 0) {
    return async_accept_awaitable(ctx, listen_fd, addr, addrlen, flags);
}

/// Create an async connect awaitable
inline auto async_connect(io_context& ctx, int fd, 
                          const struct sockaddr* addr, socklen_t addrlen) {
    return async_connect_awaitable(ctx, fd, addr, addrlen);
}

/// Create an async close awaitable
inline auto async_close(io_context& ctx, int fd) {
    return async_close_awaitable(ctx, fd);
}

/// Create an async poll awaitable for reading
inline auto async_poll_read(io_context& ctx, int fd) {
    return async_poll_awaitable(ctx, fd, true);
}

/// Create an async poll awaitable for writing
inline auto async_poll_write(io_context& ctx, int fd) {
    return async_poll_awaitable(ctx, fd, false);
}

// Convenience overloads using default io_context

inline auto async_read(int fd, void* buffer, size_t length, int64_t offset = -1) {
    return async_read(default_io_context(), fd, buffer, length, offset);
}

inline auto async_write(int fd, const void* buffer, size_t length, 
                        int64_t offset = -1) {
    return async_write(default_io_context(), fd, buffer, length, offset);
}

inline auto async_recv(int fd, void* buffer, size_t length, int flags = 0) {
    return async_recv(default_io_context(), fd, buffer, length, flags);
}

inline auto async_send(int fd, const void* buffer, size_t length, int flags = 0) {
    return async_send(default_io_context(), fd, buffer, length, flags);
}

inline auto async_accept(int listen_fd, struct sockaddr* addr = nullptr,
                         socklen_t* addrlen = nullptr, int flags = 0) {
    return async_accept(default_io_context(), listen_fd, addr, addrlen, flags);
}

inline auto async_connect(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    return async_connect(default_io_context(), fd, addr, addrlen);
}

inline auto async_close(int fd) {
    return async_close(default_io_context(), fd);
}

} // namespace elio::io
