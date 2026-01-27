#pragma once

#include <elio/io/io_context.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <optional>
#include <span>
#include <filesystem>

namespace elio::net {

/// Unix Domain Socket options
struct uds_options {
    bool reuse_addr = false;     ///< SO_REUSEADDR (less common for UDS)
    int recv_buffer = 0;         ///< SO_RCVBUF (0 = system default)
    int send_buffer = 0;         ///< SO_SNDBUF (0 = system default)
    int backlog = 128;           ///< Listen backlog
    bool unlink_on_bind = true;  ///< Unlink existing socket file before bind
};

/// Unix socket address wrapper
struct unix_address {
    std::string path;
    
    unix_address() = default;
    
    /// Construct from path string
    explicit unix_address(std::string_view p) : path(p) {}
    
    /// Construct from sockaddr_un
    explicit unix_address(const struct sockaddr_un& sa) {
        if (sa.sun_path[0] == '\0') {
            // Abstract socket (Linux-specific)
            // The actual name starts at sun_path[1]
            path = std::string(sa.sun_path, sizeof(sa.sun_path));
        } else {
            path = sa.sun_path;
        }
    }
    
    /// Convert to sockaddr_un
    struct sockaddr_un to_sockaddr() const {
        struct sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        
        if (path.size() >= sizeof(sa.sun_path)) {
            ELIO_LOG_ERROR("Unix socket path too long: {} (max {})", 
                          path.size(), sizeof(sa.sun_path) - 1);
            // Truncate to fit
            std::memcpy(sa.sun_path, path.data(), sizeof(sa.sun_path) - 1);
            sa.sun_path[sizeof(sa.sun_path) - 1] = '\0';
        } else {
            std::memcpy(sa.sun_path, path.data(), path.size());
            sa.sun_path[path.size()] = '\0';
        }
        
        return sa;
    }
    
    /// Get sockaddr length (varies for abstract sockets)
    socklen_t sockaddr_len() const {
        if (path.empty()) {
            return sizeof(sa_family_t);
        }
        if (path[0] == '\0') {
            // Abstract socket: length includes null byte and name
            return static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + path.size());
        }
        // Filesystem socket: include null terminator
        return static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + path.size() + 1);
    }
    
    /// Check if this is an abstract socket (Linux-specific)
    bool is_abstract() const {
        return !path.empty() && path[0] == '\0';
    }
    
    /// Create an abstract socket address (Linux-specific)
    /// Abstract sockets don't create filesystem entries and are automatically
    /// cleaned up when all references are closed
    static unix_address abstract(std::string_view name) {
        unix_address addr;
        addr.path.reserve(name.size() + 1);
        addr.path.push_back('\0');
        addr.path.append(name);
        return addr;
    }
    
    std::string to_string() const {
        if (path.empty()) {
            return "(unnamed)";
        }
        if (path[0] == '\0') {
            return "@" + path.substr(1);  // Convention: @ for abstract
        }
        return path;
    }
};

/// Unix Domain Socket stream for connected sockets
class uds_stream {
public:
    /// Construct from file descriptor
    explicit uds_stream(int fd, io::io_context& ctx) 
        : fd_(fd), ctx_(&ctx) {
        // Make non-blocking
        int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    }
    
    /// Move constructor
    uds_stream(uds_stream&& other) noexcept
        : fd_(other.fd_)
        , ctx_(other.ctx_)
        , peer_addr_(std::move(other.peer_addr_)) {
        other.fd_ = -1;
    }
    
    /// Move assignment
    uds_stream& operator=(uds_stream&& other) noexcept {
        if (this != &other) {
            close_sync();
            fd_ = other.fd_;
            ctx_ = other.ctx_;
            peer_addr_ = std::move(other.peer_addr_);
            other.fd_ = -1;
        }
        return *this;
    }
    
    /// Destructor
    ~uds_stream() {
        close_sync();
    }
    
    // Non-copyable
    uds_stream(const uds_stream&) = delete;
    uds_stream& operator=(const uds_stream&) = delete;
    
    /// Check if stream is valid
    bool is_valid() const noexcept { return fd_ >= 0; }
    
    /// Get the file descriptor
    int fd() const noexcept { return fd_; }
    
    /// Get the I/O context
    io::io_context& context() noexcept { return *ctx_; }
    const io::io_context& context() const noexcept { return *ctx_; }
    
    /// Get peer address
    std::optional<unix_address> peer_address() const {
        if (!peer_addr_.path.empty()) {
            return peer_addr_;
        }
        
        struct sockaddr_un sa{};
        socklen_t len = sizeof(sa);
        if (getpeername(fd_, reinterpret_cast<struct sockaddr*>(&sa), &len) == 0) {
            return unix_address(sa);
        }
        return std::nullopt;
    }
    
    /// Set peer address (used after accept)
    void set_peer_address(const unix_address& addr) {
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
    
    /// Async writev (scatter-gather write)
    auto writev(struct iovec* iovecs, size_t count) {
        return io::async_writev(*ctx_, fd_, iovecs, count);
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
    
    /// Set SO_RCVBUF option
    bool set_recv_buffer(int size) {
        return setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == 0;
    }
    
    /// Set SO_SNDBUF option
    bool set_send_buffer(int size) {
        return setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0;
    }
    
    /// Enable/disable SO_PASSCRED (receive credentials)
    bool set_pass_credentials(bool enable) {
        int flag = enable ? 1 : 0;
        return setsockopt(fd_, SOL_SOCKET, SO_PASSCRED, &flag, sizeof(flag)) == 0;
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
    unix_address peer_addr_;
};

/// Unix Domain Socket listener for accepting connections
class uds_listener {
public:
    /// Create and bind a Unix Domain Socket listener
    /// @param addr Address (path) to bind to
    /// @param ctx I/O context
    /// @param opts Socket options
    /// @return UDS listener on success, std::nullopt on error (check errno)
    static std::optional<uds_listener> bind(
        const unix_address& addr,
        io::io_context& ctx,
        const uds_options& opts = {}) 
    {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return std::nullopt;
        }
        
        // Apply socket options
        if (opts.reuse_addr) {
            int flag = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
        }
        
        if (opts.recv_buffer > 0) {
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &opts.recv_buffer, sizeof(opts.recv_buffer));
        }
        
        if (opts.send_buffer > 0) {
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &opts.send_buffer, sizeof(opts.send_buffer));
        }
        
        // Unlink existing socket file if requested (only for filesystem sockets)
        if (opts.unlink_on_bind && !addr.is_abstract() && !addr.path.empty()) {
            ::unlink(addr.path.c_str());
        }
        
        // Bind
        auto sa = addr.to_sockaddr();
        socklen_t sa_len = addr.sockaddr_len();
        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&sa), sa_len) < 0) {
            int err = errno;
            ::close(fd);
            errno = err;
            return std::nullopt;
        }
        
        // Listen
        if (::listen(fd, opts.backlog) < 0) {
            int err = errno;
            ::close(fd);
            errno = err;
            return std::nullopt;
        }
        
        ELIO_LOG_INFO("UDS listener bound to {}", addr.to_string());
        
        return uds_listener(fd, ctx, addr, opts);
    }
    
    /// Move constructor
    uds_listener(uds_listener&& other) noexcept
        : fd_(other.fd_)
        , ctx_(other.ctx_)
        , local_addr_(std::move(other.local_addr_))
        , opts_(other.opts_) {
        other.fd_ = -1;
    }
    
    /// Move assignment
    uds_listener& operator=(uds_listener&& other) noexcept {
        if (this != &other) {
            close_sync();
            fd_ = other.fd_;
            ctx_ = other.ctx_;
            local_addr_ = std::move(other.local_addr_);
            opts_ = other.opts_;
            other.fd_ = -1;
        }
        return *this;
    }
    
    /// Destructor
    ~uds_listener() {
        close_sync();
    }
    
    // Non-copyable
    uds_listener(const uds_listener&) = delete;
    uds_listener& operator=(const uds_listener&) = delete;
    
    /// Check if listener is valid
    bool is_valid() const noexcept { return fd_ >= 0; }
    
    /// Get the file descriptor
    int fd() const noexcept { return fd_; }
    
    /// Get local address
    const unix_address& local_address() const noexcept { return local_addr_; }
    
    /// Accept a connection awaitable
    class accept_awaitable {
    public:
        accept_awaitable(uds_listener& listener)
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
        
        std::optional<uds_stream> await_resume() {
            result_ = io::io_context::get_last_result();
            
            if (!result_.success()) {
                errno = result_.error_code();
                return std::nullopt;
            }
            
            int client_fd = result_.result;
            uds_stream stream(client_fd, *listener_.ctx_);
            
            // Set peer address if available
            if (peer_addr_len_ > offsetof(struct sockaddr_un, sun_path)) {
                stream.set_peer_address(unix_address(peer_addr_));
            }
            
            ELIO_LOG_DEBUG("Accepted UDS connection");
            
            return stream;
        }
        
    private:
        uds_listener& listener_;
        struct sockaddr_un peer_addr_{};
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
    uds_listener(int fd, io::io_context& ctx, const unix_address& addr, const uds_options& opts)
        : fd_(fd), ctx_(&ctx), local_addr_(addr), opts_(opts) {}
    
    void close_sync() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
            
            // Unlink socket file (only for filesystem sockets)
            if (!local_addr_.is_abstract() && !local_addr_.path.empty()) {
                ::unlink(local_addr_.path.c_str());
            }
            
            ELIO_LOG_INFO("UDS listener closed");
        }
    }
    
    int fd_ = -1;
    io::io_context* ctx_;
    unix_address local_addr_;
    uds_options opts_;
};

/// Connect to a Unix Domain Socket server
class uds_connect_awaitable {
public:
    uds_connect_awaitable(io::io_context& ctx, const unix_address& addr,
                          const uds_options& opts = {})
        : ctx_(ctx), addr_(addr), opts_(opts) {}
    
    bool await_ready() const noexcept { return false; }
    
    bool await_suspend(std::coroutine_handle<> awaiter) {
        // Create socket
        fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            result_ = io::io_result{-errno, 0};
            return false;  // Don't suspend, resume immediately
        }
        
        // Apply buffer options if specified
        if (opts_.recv_buffer > 0) {
            setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &opts_.recv_buffer, sizeof(opts_.recv_buffer));
        }
        if (opts_.send_buffer > 0) {
            setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &opts_.send_buffer, sizeof(opts_.send_buffer));
        }
        
        // Initiate non-blocking connect
        sa_ = addr_.to_sockaddr();
        sa_len_ = addr_.sockaddr_len();
        
        int ret = ::connect(fd_, reinterpret_cast<struct sockaddr*>(&sa_), sa_len_);
        if (ret == 0) {
            // Connected immediately (common for UDS)
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
        io::io_request req{};
        req.op = io::io_op::connect;
        req.fd = fd_;
        req.addr = reinterpret_cast<struct sockaddr*>(&sa_);
        req.addrlen = &sa_len_;
        req.awaiter = awaiter;
        
        if (!ctx_.prepare(req)) {
            ::close(fd_);
            fd_ = -1;
            result_ = io::io_result{-EAGAIN, 0};
            return false;  // Don't suspend, resume immediately
        }
        ctx_.submit();
        return true;  // Suspend, will be resumed by epoll
    }
    
    std::optional<uds_stream> await_resume() {
        // For async completion (EINPROGRESS path), get result from io_context
        // For immediate completion, result_ is already set
        if (result_.result == 0 && result_.flags == 0 && fd_ >= 0) {
            // This could be immediate success ({0,0}) or we need to check async result
            auto ctx_result = io::io_context::get_last_result();
            // Only use ctx_result if it looks like a real completion (not default)
            if (ctx_result.result != 0 || ctx_result.flags != 0) {
                result_ = ctx_result;
            }
            // If ctx_result is also {0,0}, keep our result_ (immediate success)
        }
        
        if (!result_.success()) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            errno = result_.error_code();
            return std::nullopt;
        }
        
        uds_stream stream(fd_, ctx_);
        fd_ = -1;  // Transfer ownership
        stream.set_peer_address(addr_);
        
        ELIO_LOG_DEBUG("Connected to {}", addr_.to_string());
        
        return stream;
    }
    
private:
    io::io_context& ctx_;
    unix_address addr_;
    uds_options opts_;
    struct sockaddr_un sa_{};
    socklen_t sa_len_ = 0;
    int fd_ = -1;
    io::io_result result_{};
};

/// Connect to a Unix Domain Socket server
inline auto uds_connect(io::io_context& ctx, const unix_address& addr,
                        const uds_options& opts = {}) {
    return uds_connect_awaitable(ctx, addr, opts);
}

/// Connect to a Unix Domain Socket server by path
inline auto uds_connect(io::io_context& ctx, std::string_view path,
                        const uds_options& opts = {}) {
    return uds_connect_awaitable(ctx, unix_address(path), opts);
}

} // namespace elio::net
