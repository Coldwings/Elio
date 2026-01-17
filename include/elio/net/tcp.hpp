#pragma once

#include <elio/io/io_context.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <optional>
#include <expected>
#include <span>

namespace elio::net {

/// TCP socket options
struct tcp_options {
    bool reuse_addr = true;      ///< SO_REUSEADDR
    bool reuse_port = false;     ///< SO_REUSEPORT
    bool no_delay = true;        ///< TCP_NODELAY (disable Nagle's algorithm)
    bool keep_alive = false;     ///< SO_KEEPALIVE
    int recv_buffer = 0;         ///< SO_RCVBUF (0 = system default)
    int send_buffer = 0;         ///< SO_SNDBUF (0 = system default)
    int backlog = 128;           ///< Listen backlog
};

/// IPv4 address wrapper
struct ipv4_address {
    uint32_t addr = INADDR_ANY;
    uint16_t port = 0;
    
    ipv4_address() = default;
    
    ipv4_address(uint16_t p) : port(p) {}
    
    /// Construct from IP address string and port
    ipv4_address(std::string_view ip, uint16_t p) : port(p) {
        if (ip.empty() || ip == "0.0.0.0") {
            addr = INADDR_ANY;
        } else {
            // First try as numeric IP
            if (inet_pton(AF_INET, std::string(ip).c_str(), &addr) != 1) {
                // Not a numeric IP, try DNS resolution
                struct addrinfo hints{};
                struct addrinfo* result = nullptr;
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                
                std::string ip_str(ip);
                if (getaddrinfo(ip_str.c_str(), nullptr, &hints, &result) == 0 && result) {
                    auto* sa = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
                    addr = sa->sin_addr.s_addr;
                    freeaddrinfo(result);
                } else {
                    ELIO_LOG_ERROR("Failed to resolve hostname: {}", ip);
                    addr = INADDR_ANY;
                }
            }
        }
    }
    
    ipv4_address(const struct sockaddr_in& sa) 
        : addr(sa.sin_addr.s_addr), port(ntohs(sa.sin_port)) {}
    
    struct sockaddr_in to_sockaddr() const {
        struct sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = addr;
        sa.sin_port = htons(port);
        return sa;
    }
    
    std::string to_string() const {
        char buf[INET_ADDRSTRLEN];
        struct in_addr in{};
        in.s_addr = addr;
        inet_ntop(AF_INET, &in, buf, sizeof(buf));
        return std::string(buf) + ":" + std::to_string(port);
    }
};

/// TCP stream for connected sockets
class tcp_stream {
public:
    /// Construct from file descriptor
    explicit tcp_stream(int fd, io::io_context& ctx) 
        : fd_(fd), ctx_(&ctx) {
        // Make non-blocking
        int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    }
    
    /// Move constructor
    tcp_stream(tcp_stream&& other) noexcept
        : fd_(other.fd_)
        , ctx_(other.ctx_)
        , peer_addr_(other.peer_addr_) {
        other.fd_ = -1;
    }
    
    /// Move assignment
    tcp_stream& operator=(tcp_stream&& other) noexcept {
        if (this != &other) {
            close_sync();
            fd_ = other.fd_;
            ctx_ = other.ctx_;
            peer_addr_ = other.peer_addr_;
            other.fd_ = -1;
        }
        return *this;
    }
    
    /// Destructor
    ~tcp_stream() {
        close_sync();
    }
    
    // Non-copyable
    tcp_stream(const tcp_stream&) = delete;
    tcp_stream& operator=(const tcp_stream&) = delete;
    
    /// Check if stream is valid
    bool is_valid() const noexcept { return fd_ >= 0; }
    
    /// Get the file descriptor
    int fd() const noexcept { return fd_; }
    
    /// Get the I/O context
    io::io_context& context() noexcept { return *ctx_; }
    const io::io_context& context() const noexcept { return *ctx_; }
    
    /// Get peer address
    std::optional<ipv4_address> peer_address() const {
        if (peer_addr_.port != 0) {
            return peer_addr_;
        }
        
        struct sockaddr_in sa{};
        socklen_t len = sizeof(sa);
        if (getpeername(fd_, reinterpret_cast<struct sockaddr*>(&sa), &len) == 0) {
            return ipv4_address(sa);
        }
        return std::nullopt;
    }
    
    /// Set peer address (used after accept)
    void set_peer_address(const ipv4_address& addr) {
        peer_addr_ = addr;
    }
    
    /// Async read
    auto read(void* buffer, size_t length) {
        return io::async_recv(*ctx_, fd_, buffer, length);
    }
    
    /// Async read into span
    template<typename T>
    auto read(std::span<T> buffer) {
        return io::async_recv(*ctx_, fd_, buffer.data(), buffer.size_bytes());
    }
    
    /// Async write
    auto write(const void* buffer, size_t length) {
        return io::async_send(*ctx_, fd_, buffer, length);
    }
    
    /// Async write from span
    template<typename T>
    auto write(std::span<const T> buffer) {
        return io::async_send(*ctx_, fd_, buffer.data(), buffer.size_bytes());
    }
    
    /// Async write string
    auto write(std::string_view str) {
        return io::async_send(*ctx_, fd_, str.data(), str.size());
    }
    
    /// Wait for socket to be readable
    auto poll_read() {
        return io::async_poll_read(*ctx_, fd_);
    }
    
    /// Wait for socket to be writable
    auto poll_write() {
        return io::async_poll_write(*ctx_, fd_);
    }
    
    /// Async close
    auto close() {
        int fd = fd_;
        fd_ = -1;
        return io::async_close(*ctx_, fd);
    }
    
    /// Set TCP_NODELAY option
    bool set_no_delay(bool enable) {
        int flag = enable ? 1 : 0;
        return setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == 0;
    }
    
    /// Set SO_KEEPALIVE option
    bool set_keep_alive(bool enable) {
        int flag = enable ? 1 : 0;
        return setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag)) == 0;
    }
    
private:
    void close_sync() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    
    int fd_ = -1;
    io::io_context* ctx_;
    ipv4_address peer_addr_;
};

/// TCP listener for accepting connections
class tcp_listener {
public:
    /// Create and bind a TCP listener
    /// @param addr Address to bind to
    /// @param ctx I/O context
    /// @param opts Socket options
    static std::expected<tcp_listener, int> bind(
        const ipv4_address& addr,
        io::io_context& ctx,
        const tcp_options& opts = {}) 
    {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return std::unexpected(errno);
        }
        
        // Apply socket options
        if (opts.reuse_addr) {
            int flag = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
        }
        
        if (opts.reuse_port) {
            int flag = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag));
        }
        
        if (opts.recv_buffer > 0) {
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &opts.recv_buffer, sizeof(opts.recv_buffer));
        }
        
        if (opts.send_buffer > 0) {
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &opts.send_buffer, sizeof(opts.send_buffer));
        }
        
        // Bind
        auto sa = addr.to_sockaddr();
        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0) {
            int err = errno;
            ::close(fd);
            return std::unexpected(err);
        }
        
        // Listen
        if (::listen(fd, opts.backlog) < 0) {
            int err = errno;
            ::close(fd);
            return std::unexpected(err);
        }
        
        ELIO_LOG_INFO("TCP listener bound to {}", addr.to_string());
        
        return tcp_listener(fd, ctx, addr, opts);
    }
    
    /// Move constructor
    tcp_listener(tcp_listener&& other) noexcept
        : fd_(other.fd_)
        , ctx_(other.ctx_)
        , local_addr_(other.local_addr_)
        , opts_(other.opts_) {
        other.fd_ = -1;
    }
    
    /// Move assignment
    tcp_listener& operator=(tcp_listener&& other) noexcept {
        if (this != &other) {
            close_sync();
            fd_ = other.fd_;
            ctx_ = other.ctx_;
            local_addr_ = other.local_addr_;
            opts_ = other.opts_;
            other.fd_ = -1;
        }
        return *this;
    }
    
    /// Destructor
    ~tcp_listener() {
        close_sync();
    }
    
    // Non-copyable
    tcp_listener(const tcp_listener&) = delete;
    tcp_listener& operator=(const tcp_listener&) = delete;
    
    /// Check if listener is valid
    bool is_valid() const noexcept { return fd_ >= 0; }
    
    /// Get the file descriptor
    int fd() const noexcept { return fd_; }
    
    /// Get local address
    const ipv4_address& local_address() const noexcept { return local_addr_; }
    
    /// Accept a connection awaitable
    class accept_awaitable {
    public:
        accept_awaitable(tcp_listener& listener)
            : listener_(listener) {}
        
        bool await_ready() const noexcept { return false; }
        
        void await_suspend(std::coroutine_handle<> awaiter) {
            io::io_request req{};
            req.op = io::io_op::accept;
            req.fd = listener_.fd_;
            req.addr = reinterpret_cast<struct sockaddr*>(&peer_addr_);
            req.addrlen = &peer_addr_len_;
            req.socket_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
            req.awaiter = awaiter;
            
            if (!listener_.ctx_->prepare(req)) {
                result_ = io::io_result{-EAGAIN, 0};
                awaiter.resume();
                return;
            }
            listener_.ctx_->submit();
        }
        
        std::expected<tcp_stream, int> await_resume() {
            result_ = io::io_context::get_last_result();
            
            if (!result_.success()) {
                return std::unexpected(result_.error_code());
            }
            
            int client_fd = result_.result;
            tcp_stream stream(client_fd, *listener_.ctx_);
            
            // Apply TCP options
            if (listener_.opts_.no_delay) {
                stream.set_no_delay(true);
            }
            if (listener_.opts_.keep_alive) {
                stream.set_keep_alive(true);
            }
            
            // Set peer address
            stream.set_peer_address(ipv4_address(peer_addr_));
            
            ELIO_LOG_DEBUG("Accepted connection from {}", 
                          ipv4_address(peer_addr_).to_string());
            
            return stream;
        }
        
    private:
        tcp_listener& listener_;
        struct sockaddr_in peer_addr_{};
        socklen_t peer_addr_len_ = sizeof(peer_addr_);
        io::io_result result_{};
    };
    
    /// Accept a new connection
    auto accept() {
        return accept_awaitable(*this);
    }
    
    /// Close the listener
    void close() {
        close_sync();
    }
    
private:
    tcp_listener(int fd, io::io_context& ctx, const ipv4_address& addr, const tcp_options& opts)
        : fd_(fd), ctx_(&ctx), local_addr_(addr), opts_(opts) {}
    
    void close_sync() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
            ELIO_LOG_INFO("TCP listener closed");
        }
    }
    
    int fd_ = -1;
    io::io_context* ctx_;
    ipv4_address local_addr_;
    tcp_options opts_;
};

/// Connect to a remote TCP server
class tcp_connect_awaitable {
public:
    tcp_connect_awaitable(io::io_context& ctx, const ipv4_address& addr, 
                          const tcp_options& opts = {})
        : ctx_(ctx), addr_(addr), opts_(opts) {}
    
    bool await_ready() const noexcept { return false; }
    
    void await_suspend(std::coroutine_handle<> awaiter) {
        // Create socket
        fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            result_ = io::io_result{-errno, 0};
            awaiter.resume();
            return;
        }
        
        // Apply options
        if (opts_.no_delay) {
            int flag = 1;
            setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        }
        
        // Initiate connect
        sa_ = addr_.to_sockaddr();
        
        io::io_request req{};
        req.op = io::io_op::connect;
        req.fd = fd_;
        req.addr = reinterpret_cast<struct sockaddr*>(&sa_);
        socklen_t len = sizeof(sa_);
        req.addrlen = &len;
        req.awaiter = awaiter;
        
        if (!ctx_.prepare(req)) {
            ::close(fd_);
            fd_ = -1;
            result_ = io::io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx_.submit();
    }
    
    std::expected<tcp_stream, int> await_resume() {
        result_ = io::io_context::get_last_result();
        
        if (!result_.success()) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            return std::unexpected(result_.error_code());
        }
        
        tcp_stream stream(fd_, ctx_);
        fd_ = -1;  // Transfer ownership
        stream.set_peer_address(addr_);
        
        ELIO_LOG_DEBUG("Connected to {}", addr_.to_string());
        
        return stream;
    }
    
private:
    io::io_context& ctx_;
    ipv4_address addr_;
    tcp_options opts_;
    struct sockaddr_in sa_{};
    int fd_ = -1;
    io::io_result result_{};
};

/// Connect to a remote TCP server
inline auto tcp_connect(io::io_context& ctx, const ipv4_address& addr,
                        const tcp_options& opts = {}) {
    return tcp_connect_awaitable(ctx, addr, opts);
}

/// Connect to a remote TCP server by host and port
inline auto tcp_connect(io::io_context& ctx, std::string_view host, uint16_t port,
                        const tcp_options& opts = {}) {
    return tcp_connect_awaitable(ctx, ipv4_address(host, port), opts);
}

} // namespace elio::net
