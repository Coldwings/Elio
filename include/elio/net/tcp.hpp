#pragma once

#include <elio/io/io_context.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <optional>
#include <span>
#include <variant>

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
    bool ipv6_only = false;      ///< IPV6_V6ONLY (disable dual-stack on IPv6 sockets)
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
    
    int family() const noexcept { return AF_INET; }
    
    std::string to_string() const {
        char buf[INET_ADDRSTRLEN];
        struct in_addr in{};
        in.s_addr = addr;
        inet_ntop(AF_INET, &in, buf, sizeof(buf));
        return std::string(buf) + ":" + std::to_string(port);
    }
};

/// IPv6 address wrapper
struct ipv6_address {
    struct in6_addr addr = IN6ADDR_ANY_INIT;
    uint16_t port = 0;
    uint32_t scope_id = 0;  ///< For link-local addresses
    
    ipv6_address() = default;
    
    ipv6_address(uint16_t p) : port(p) {}
    
    /// Construct from IPv6 address string and port
    ipv6_address(std::string_view ip, uint16_t p) : port(p) {
        if (ip.empty() || ip == "::") {
            addr = IN6ADDR_ANY_INIT;
        } else {
            // Handle scope ID for link-local (e.g., "fe80::1%eth0")
            std::string ip_str(ip);
            size_t scope_pos = ip_str.find('%');
            if (scope_pos != std::string::npos) {
                std::string scope_name = ip_str.substr(scope_pos + 1);
                ip_str = ip_str.substr(0, scope_pos);
                scope_id = if_nametoindex(scope_name.c_str());
            }
            
            // First try as numeric IP
            if (inet_pton(AF_INET6, ip_str.c_str(), &addr) != 1) {
                // Not a numeric IP, try DNS resolution
                struct addrinfo hints{};
                struct addrinfo* result = nullptr;
                hints.ai_family = AF_INET6;
                hints.ai_socktype = SOCK_STREAM;
                
                if (getaddrinfo(ip_str.c_str(), nullptr, &hints, &result) == 0 && result) {
                    auto* sa = reinterpret_cast<struct sockaddr_in6*>(result->ai_addr);
                    addr = sa->sin6_addr;
                    scope_id = sa->sin6_scope_id;
                    freeaddrinfo(result);
                } else {
                    ELIO_LOG_ERROR("Failed to resolve IPv6 hostname: {}", ip);
                    addr = IN6ADDR_ANY_INIT;
                }
            }
        }
    }
    
    ipv6_address(const struct sockaddr_in6& sa) 
        : addr(sa.sin6_addr), port(ntohs(sa.sin6_port)), scope_id(sa.sin6_scope_id) {}
    
    // GCC false positive with std::variant: when socket_address::to_sockaddr()
    // is inlined for an ipv4 variant, GCC thinks ipv6 fields may be uninitialized.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
    struct sockaddr_in6 to_sockaddr() const {
        struct sockaddr_in6 sa{};
        sa.sin6_family = AF_INET6;
        sa.sin6_addr = addr;
        sa.sin6_port = htons(port);
        sa.sin6_scope_id = scope_id;
        return sa;
    }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    
    int family() const noexcept { return AF_INET6; }
    
    std::string to_string() const {
        char buf[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &addr, buf, sizeof(buf));
        std::string result;
        result.reserve(INET6_ADDRSTRLEN + 10);
        result.append("[");
        result.append(buf);
        result.append("]:");
        result.append(std::to_string(port));
        return result;
    }
    
    /// Check if this is an IPv4-mapped IPv6 address (::ffff:x.x.x.x)
    bool is_v4_mapped() const {
        return IN6_IS_ADDR_V4MAPPED(&addr);
    }
};

/// Generic socket address that can hold IPv4 or IPv6
class socket_address {
public:
    socket_address() : data_(ipv4_address{}) {}
    
    socket_address(const ipv4_address& addr) : data_(addr) {}
    socket_address(const ipv6_address& addr) : data_(addr) {}
    
    /// Construct from port only (binds to all interfaces, IPv6 with dual-stack)
    explicit socket_address(uint16_t port) : data_(ipv6_address(port)) {}
    
    /// Construct from host string and port (auto-detects IPv4/IPv6)
    socket_address(std::string_view host, uint16_t port) {
        if (host.empty() || host == "::" || host == "0.0.0.0") {
            // Default to IPv6 with dual-stack
            data_ = ipv6_address(port);
            return;
        }
        
        // Check if it looks like an IPv6 address
        if (host.find(':') != std::string_view::npos) {
            data_ = ipv6_address(host, port);
            return;
        }
        
        // Try to resolve and prefer IPv6
        struct addrinfo hints{};
        struct addrinfo* result = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        
        std::string host_str(host);
        if (getaddrinfo(host_str.c_str(), nullptr, &hints, &result) == 0 && result) {
            // Use the first result
            if (result->ai_family == AF_INET6) {
                auto* sa = reinterpret_cast<struct sockaddr_in6*>(result->ai_addr);
                ipv6_address addr(*sa);
                addr.port = port;
                data_ = addr;
            } else {
                auto* sa = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
                ipv4_address addr(*sa);
                addr.port = port;
                data_ = addr;
            }
            freeaddrinfo(result);
        } else {
            // Fallback to IPv4
            data_ = ipv4_address(host, port);
        }
    }
    
    /// Construct from sockaddr_storage
    socket_address(const struct sockaddr_storage& ss) {
        if (ss.ss_family == AF_INET) {
            data_ = ipv4_address(*reinterpret_cast<const struct sockaddr_in*>(&ss));
        } else if (ss.ss_family == AF_INET6) {
            data_ = ipv6_address(*reinterpret_cast<const struct sockaddr_in6*>(&ss));
        }
    }
    
    /// Get address family
    int family() const {
        return std::visit([](const auto& addr) { return addr.family(); }, data_);
    }
    
    /// Get port
    uint16_t port() const {
        return std::visit([](const auto& addr) { return addr.port; }, data_);
    }
    
    /// Check if this is an IPv4 address
    bool is_v4() const { return std::holds_alternative<ipv4_address>(data_); }
    
    /// Check if this is an IPv6 address
    bool is_v6() const { return std::holds_alternative<ipv6_address>(data_); }
    
    /// Get as IPv4 address (throws if not IPv4)
    const ipv4_address& as_v4() const { return std::get<ipv4_address>(data_); }
    
    /// Get as IPv6 address (throws if not IPv6)
    const ipv6_address& as_v6() const { return std::get<ipv6_address>(data_); }
    
    /// Fill sockaddr_storage
    socklen_t to_sockaddr(struct sockaddr_storage& ss) const {
        std::memset(&ss, 0, sizeof(ss));
        return std::visit([&ss](const auto& addr) -> socklen_t {
            auto sa = addr.to_sockaddr();
            std::memcpy(&ss, &sa, sizeof(sa));
            return sizeof(sa);
        }, data_);
    }
    
    std::string to_string() const {
        return std::visit([](const auto& addr) { return addr.to_string(); }, data_);
    }
    
private:
    std::variant<ipv4_address, ipv6_address> data_;
};

/// TCP stream for connected sockets
class tcp_stream {
public:
    /// Construct from file descriptor
    explicit tcp_stream(int fd)
        : fd_(fd) {
        // Make non-blocking
        int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    }

    /// Move constructor
    tcp_stream(tcp_stream&& other) noexcept
        : fd_(other.fd_)
        , peer_addr_(std::move(other.peer_addr_)) {
        other.fd_ = -1;
    }

    /// Move assignment
    tcp_stream& operator=(tcp_stream&& other) noexcept {
        if (this != &other) {
            close_sync();
            fd_ = other.fd_;
            peer_addr_ = std::move(other.peer_addr_);
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
    
    /// Get peer address
    std::optional<socket_address> peer_address() const {
        if (peer_addr_) {
            return peer_addr_;
        }
        
        struct sockaddr_storage ss{};
        socklen_t len = sizeof(ss);
        if (getpeername(fd_, reinterpret_cast<struct sockaddr*>(&ss), &len) == 0) {
            return socket_address(ss);
        }
        return std::nullopt;
    }
    
    /// Set peer address (used after accept)
    void set_peer_address(const socket_address& addr) {
        peer_addr_ = addr;
    }
    
    /// Set peer address from IPv4
    void set_peer_address(const ipv4_address& addr) {
        peer_addr_ = socket_address(addr);
    }
    
    /// Set peer address from IPv6
    void set_peer_address(const ipv6_address& addr) {
        peer_addr_ = socket_address(addr);
    }
    
    /// Async read
    auto read(void* buffer, size_t length) {
        return io::async_recv(fd_, buffer, length);
    }
    
    /// Async read into span
    template<typename T>
    auto read(std::span<T> buffer) {
        return io::async_recv(fd_, buffer.data(), buffer.size_bytes());
    }
    
    /// Async write
    auto write(const void* buffer, size_t length) {
        return io::async_send(fd_, buffer, length);
    }
    
    /// Async write from span
    template<typename T>
    auto write(std::span<const T> buffer) {
        return io::async_send(fd_, buffer.data(), buffer.size_bytes());
    }
    
    /// Async write string
    auto write(std::string_view str) {
        return io::async_send(fd_, str.data(), str.size());
    }
    
    /// Async writev (scatter-gather write)
    auto writev(struct iovec* iovecs, size_t count) {
        return io::async_writev(fd_, iovecs, count);
    }
    
    /// Wait for socket to be readable
    auto poll_read() {
        return io::async_poll_read(fd_);
    }
    
    /// Wait for socket to be writable
    auto poll_write() {
        return io::async_poll_write(fd_);
    }
    
    /// Async close
    auto close() {
        int fd = fd_;
        fd_ = -1;
        return io::async_close(fd);
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
    std::optional<socket_address> peer_addr_;
};

/// TCP listener for accepting connections
class tcp_listener {
public:
    /// Create and bind a TCP listener (IPv4)
    /// @param addr Address to bind to
    /// @param opts Socket options
    /// @return TCP listener on success, std::nullopt on error (check errno)
    static std::optional<tcp_listener> bind(
        const ipv4_address& addr,
        const tcp_options& opts = {})
    {
        return bind_impl(socket_address(addr), opts);
    }

    /// Create and bind a TCP listener (IPv6)
    static std::optional<tcp_listener> bind(
        const ipv6_address& addr,
        const tcp_options& opts = {})
    {
        return bind_impl(socket_address(addr), opts);
    }

    /// Create and bind a TCP listener (generic address)
    static std::optional<tcp_listener> bind(
        const socket_address& addr,
        const tcp_options& opts = {})
    {
        return bind_impl(addr, opts);
    }
    
    /// Move constructor
    tcp_listener(tcp_listener&& other) noexcept
        : fd_(other.fd_)
        , local_addr_(std::move(other.local_addr_))
        , opts_(other.opts_) {
        other.fd_ = -1;
    }

    /// Move assignment
    tcp_listener& operator=(tcp_listener&& other) noexcept {
        if (this != &other) {
            close_sync();
            fd_ = other.fd_;
            local_addr_ = std::move(other.local_addr_);
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
    const socket_address& local_address() const noexcept { return local_addr_; }
    
    /// Accept a connection awaitable
    class accept_awaitable {
    public:
        accept_awaitable(tcp_listener& listener)
            : listener_(listener) {}
        
        bool await_ready() const noexcept { return false; }
        
        void await_suspend(std::coroutine_handle<> awaiter) {
            auto& ctx = io::current_io_context();
            
            io::io_request req{};
            req.op = io::io_op::accept;
            req.fd = listener_.fd_;
            req.addr = reinterpret_cast<struct sockaddr*>(&peer_addr_);
            req.addrlen = &peer_addr_len_;
            req.socket_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
            req.awaiter = awaiter;
            
            if (!ctx.prepare(req)) {
                result_ = io::io_result{-EAGAIN, 0};
                awaiter.resume();
                return;
            }
            ctx.submit();
        }
        
        std::optional<tcp_stream> await_resume() {
            result_ = io::io_context::get_last_result();

            if (!result_.success()) {
                errno = result_.error_code();
                return std::nullopt;
            }

            int client_fd = result_.result;
            tcp_stream stream(client_fd);
            
            // Apply TCP options
            if (listener_.opts_.no_delay) {
                stream.set_no_delay(true);
            }
            if (listener_.opts_.keep_alive) {
                stream.set_keep_alive(true);
            }
            
            // Set peer address
            socket_address peer(peer_addr_);
            stream.set_peer_address(peer);
            
            ELIO_LOG_DEBUG("Accepted connection from {}", peer.to_string());
            
            return stream;
        }
        
    private:
        tcp_listener& listener_;
        struct sockaddr_storage peer_addr_{};
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
    static std::optional<tcp_listener> bind_impl(
        const socket_address& addr,
        const tcp_options& opts)
    {
        int family = addr.family();
        int fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return std::nullopt;
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
        
        // IPv6-specific options
        if (family == AF_INET6) {
            int flag = opts.ipv6_only ? 1 : 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &flag, sizeof(flag));
        }
        
        // Bind
        struct sockaddr_storage ss{};
        socklen_t ss_len = addr.to_sockaddr(ss);
        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&ss), ss_len) < 0) {
            int err = errno;
            ::close(fd);
            errno = err;
            return std::nullopt;
        }
        
        // Query actual bound address (important when binding to port 0)
        socket_address bound_addr = addr;
        struct sockaddr_storage bound_ss{};
        socklen_t bound_len = sizeof(bound_ss);
        if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&bound_ss), &bound_len) == 0) {
            bound_addr = socket_address(bound_ss);
        }
        
        // Listen
        if (::listen(fd, opts.backlog) < 0) {
            int err = errno;
            ::close(fd);
            errno = err;
            return std::nullopt;
        }
        
        ELIO_LOG_INFO("TCP listener bound to {}", bound_addr.to_string());

        return tcp_listener(fd, bound_addr, opts);
    }

    tcp_listener(int fd, const socket_address& addr, const tcp_options& opts)
        : fd_(fd), local_addr_(addr), opts_(opts) {}
    
    void close_sync() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
            ELIO_LOG_INFO("TCP listener closed");
        }
    }
    
    int fd_ = -1;
    socket_address local_addr_;
    tcp_options opts_;
};

/// Connect to a remote TCP server
class tcp_connect_awaitable {
public:
    tcp_connect_awaitable(const socket_address& addr, 
                          const tcp_options& opts = {})
        : addr_(addr), opts_(opts) {}
    
    bool await_ready() const noexcept { return false; }
    
    bool await_suspend(std::coroutine_handle<> awaiter) {
        // Create socket with appropriate address family
        fd_ = socket(addr_.family(), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            result_ = io::io_result{-errno, 0};
            return false;  // Don't suspend, resume immediately
        }
        
        // Apply options
        if (opts_.no_delay) {
            int flag = 1;
            setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        }
        
        // Get sockaddr
        socklen_t sa_len = addr_.to_sockaddr(sa_);
        
        int ret = ::connect(fd_, reinterpret_cast<struct sockaddr*>(&sa_), sa_len);
        if (ret == 0) {
            // Connected immediately (rare for TCP, but possible)
            result_ = io::io_result{0, 0};
            return false;  // Don't suspend, resume immediately
        }
        
        if (errno != EINPROGRESS) {
            // Connection failed
            result_ = io::io_result{-errno, 0};
            ::close(fd_);
            fd_ = -1;
            return false;  // Don't suspend, resume immediately
        }
        
        // Connection in progress, wait for socket to become writable
        auto& ctx = io::current_io_context();
        
        io::io_request req{};
        req.op = io::io_op::connect;
        req.fd = fd_;
        req.addr = reinterpret_cast<struct sockaddr*>(&sa_);
        req.addrlen = &sa_len_;
        req.awaiter = awaiter;
        
        if (!ctx.prepare(req)) {
            ::close(fd_);
            fd_ = -1;
            result_ = io::io_result{-EAGAIN, 0};
            return false;  // Don't suspend, resume immediately
        }
        ctx.submit();
        return true;  // Suspend, will be resumed by epoll
    }
    
    std::optional<tcp_stream> await_resume() {
        // If result wasn't set (async path completed), get from io_context
        if (result_.result == 0 && fd_ >= 0) {
            auto ctx_result = io::io_context::get_last_result();
            if (ctx_result.result != 0 || ctx_result.flags != 0) {
                result_ = ctx_result;
            }
        }
        
        if (!result_.success()) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            errno = result_.error_code();
            return std::nullopt;
        }
        
        tcp_stream stream(fd_);
        fd_ = -1;  // Transfer ownership
        stream.set_peer_address(addr_);
        
        ELIO_LOG_DEBUG("Connected to {}", addr_.to_string());
        
        return stream;
    }
    
private:
    socket_address addr_;
    tcp_options opts_;
    struct sockaddr_storage sa_{};
    socklen_t sa_len_ = sizeof(sa_);
    int fd_ = -1;
    io::io_result result_{};
};

/// Connect to a remote TCP server (IPv4)
inline auto tcp_connect(const ipv4_address& addr,
                        const tcp_options& opts = {}) {
    return tcp_connect_awaitable(socket_address(addr), opts);
}

/// Connect to a remote TCP server (IPv6)
inline auto tcp_connect(const ipv6_address& addr,
                        const tcp_options& opts = {}) {
    return tcp_connect_awaitable(socket_address(addr), opts);
}

/// Connect to a remote TCP server (generic address)
inline auto tcp_connect(const socket_address& addr,
                        const tcp_options& opts = {}) {
    return tcp_connect_awaitable(addr, opts);
}

/// Connect to a remote TCP server by host and port (auto-detects IPv4/IPv6)
inline auto tcp_connect(std::string_view host, uint16_t port,
                        const tcp_options& opts = {}) {
    return tcp_connect_awaitable(socket_address(host, port), opts);
}

} // namespace elio::net
