#pragma once

#include <elio/io/io_context.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
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
            // The actual name starts at sun_path[1]. Trim trailing null bytes
            // since the caller may not have provided the exact sockaddr length.
            size_t len = sizeof(sa.sun_path);
            while (len > 1 && sa.sun_path[len - 1] == '\0') {
                --len;
            }
            path = std::string(sa.sun_path, len);
        } else {
            path = sa.sun_path;
        }
    }

    /// Construct from sockaddr_un with explicit length (preferred for abstract sockets)
    unix_address(const struct sockaddr_un& sa, socklen_t sa_len) {
        if (sa.sun_path[0] == '\0') {
            // Abstract socket: use the exact length from the kernel
            size_t name_len = sa_len - offsetof(struct sockaddr_un, sun_path);
            if (name_len > sizeof(sa.sun_path)) {
                name_len = sizeof(sa.sun_path);
            }
            path = std::string(sa.sun_path, name_len);
        } else {
            path = sa.sun_path;
        }
    }
    
    /// Convert to sockaddr_un
    /// @throws std::invalid_argument if the path exceeds sun_path capacity
    struct sockaddr_un to_sockaddr() const {
        struct sockaddr_un sa{};
        sa.sun_family = AF_UNIX;

        // For abstract sockets (path[0] == '\0'), the full path.size() bytes
        // must fit in sun_path. For filesystem sockets, we need room for a
        // null terminator.
        size_t max_len = is_abstract() ? sizeof(sa.sun_path) : sizeof(sa.sun_path) - 1;
        if (path.size() > max_len) {
            throw std::invalid_argument(
                "unix_address: path too long (" + std::to_string(path.size()) +
                " bytes, max " + std::to_string(max_len) + ")");
        }
        std::memcpy(sa.sun_path, path.data(), path.size());
        if (!is_abstract()) {
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
            // Convention: @ for abstract socket names
            std::string result("@");
            result.append(path, 1);
            return result;
        }
        return path;
    }
};

/// Unix Domain Socket stream for connected sockets
///
/// **Thread safety:** a ``uds_stream`` is **not** safe for arbitrary
/// concurrent use. The supported pattern is one reader and one writer in
/// distinct coroutines (matching socket full-duplex semantics): concurrent
/// ``read``/``poll_read`` and concurrent ``write``/``writev``/``poll_write``
/// in different coroutines is well-defined. Issuing two concurrent reads,
/// two concurrent writes, or a read alongside a ``close`` on the same
/// instance is undefined behaviour unless externally serialised. The same
/// contract applies to ``uds_listener::accept``: only one coroutine may be
/// accepting at a time.
class uds_stream {
public:
    /// Construct from file descriptor
    explicit uds_stream(int fd)
        : fd_(fd) {
        // Make non-blocking
        int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    }

    /// Move constructor
    uds_stream(uds_stream&& other) noexcept
        : fd_(other.fd_)
        , peer_addr_(std::move(other.peer_addr_)) {
        other.fd_ = -1;
    }

    /// Move assignment
    uds_stream& operator=(uds_stream&& other) noexcept {
        if (this != &other) {
            close_sync();
            fd_ = other.fd_;
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
    
    /// Async read.
    ///
    /// Waits for the socket to become readable and retries on transient
    /// readiness errors (-EAGAIN/-EWOULDBLOCK) or interruption (-EINTR)
    /// rather than surfacing them to the caller. A positive short read is
    /// returned as-is; a hard error or a poll failure is returned unchanged.
    coro::task<io::io_result> read(void* buffer, size_t length) {
        while (true) {
            auto result = co_await io::async_recv(fd_, buffer, length);
            if (result.result == -EAGAIN || result.result == -EWOULDBLOCK) {
                auto ready = co_await io::async_poll_read(fd_);
                if (ready.result < 0) {
                    co_return ready;
                }
                continue;
            }
            if (result.result == -EINTR) {
                continue;
            }
            co_return result;
        }
    }
    
    /// Async read into span
    template<typename T>
    coro::task<io::io_result> read(std::span<T> buffer) {
        return read(buffer.data(), buffer.size_bytes());
    }
    
    /// Async write.
    ///
    /// Waits for the socket to become writable and retries on transient
    /// readiness errors (-EAGAIN/-EWOULDBLOCK) or interruption (-EINTR)
    /// rather than surfacing them to the caller. A positive short write is
    /// returned as-is; a hard error or a poll failure is returned unchanged.
    coro::task<io::io_result> write(const void* buffer, size_t length) {
        while (true) {
            auto result = co_await io::async_send(fd_, buffer, length);
            if (result.result == -EAGAIN || result.result == -EWOULDBLOCK) {
                auto ready = co_await io::async_poll_write(fd_);
                if (ready.result < 0) {
                    co_return ready;
                }
                continue;
            }
            if (result.result == -EINTR) {
                continue;
            }
            co_return result;
        }
    }

    /// Async write, cancellable by ``token``.
    coro::task<io::io_result> write(const void* buffer, size_t length,
                                    coro::cancel_token token) {
        while (true) {
            if (token.is_cancelled()) {
                co_return io::io_result{-ECANCELED, 0};
            }

            auto result = co_await io::async_send(fd_, buffer, length, 0, token);
            if (result.was_cancelled() || result.io.result == -ECANCELED) {
                co_return io::io_result{-ECANCELED, 0};
            }
            if (result.io.result == -EAGAIN ||
                result.io.result == -EWOULDBLOCK) {
                auto ready = co_await io::async_poll_write(fd_, token);
                if (ready.was_cancelled() || ready.io.result == -ECANCELED) {
                    co_return io::io_result{-ECANCELED, 0};
                }
                if (ready.io.result < 0) {
                    co_return ready.io;
                }
                continue;
            }
            if (result.io.result == -EINTR) {
                continue;
            }
            co_return result.io;
        }
    }
    
    /// Async write from span
    template<typename T>
    coro::task<io::io_result> write(std::span<const T> buffer) {
        return write(buffer.data(), buffer.size_bytes());
    }

    /// Async write from span, cancellable by ``token``.
    template<typename T>
    coro::task<io::io_result> write(std::span<const T> buffer,
                                    coro::cancel_token token) {
        return write(buffer.data(), buffer.size_bytes(), std::move(token));
    }
    
    /// Async write string
    coro::task<io::io_result> write(std::string_view str) {
        return write(str.data(), str.size());
    }

    /// Async write string, cancellable by ``token``.
    coro::task<io::io_result> write(std::string_view str,
                                    coro::cancel_token token) {
        return write(str.data(), str.size(), std::move(token));
    }
    
    /// Async writev (scatter-gather write)
    auto writev(struct iovec* iovecs, size_t count) {
        return io::async_sendmsg(fd_, iovecs, count);
    }
    
    /// Wait for socket to be readable
    auto poll_read() {
        return io::async_poll_read(fd_);
    }

    /// Wait for socket to be readable, cancellable by ``token``
    auto poll_read(coro::cancel_token token) {
        return io::async_poll_read(fd_, std::move(token));
    }
    
    /// Wait for socket to be writable
    auto poll_write() {
        return io::async_poll_write(fd_);
    }

    /// Wait for socket to be writable, cancellable by ``token``
    auto poll_write(coro::cancel_token token) {
        return io::async_poll_write(fd_, std::move(token));
    }

    /// Read exactly ``length`` bytes into ``buffer``.
    ///
    /// Loops over partial reads until ``length`` bytes have been stored, a
    /// terminal error occurs, or the peer closes the connection (EOF). A
    /// transient readiness error (``-EAGAIN`` / ``-EWOULDBLOCK``) is not
    /// surfaced: the stream waits for the socket to become readable and
    /// retries.
    ///
    /// @return ``io_result`` whose ``result`` is ``length`` on success. If
    ///         the peer closes before ``length`` bytes arrive, returns
    ///         ``-ENODATA`` (short read / unexpected EOF). Any other terminal
    ///         error from the underlying ``read`` is returned as-is.
    coro::task<io::io_result> read_exactly(void* buffer, size_t length) {
        if (length > static_cast<size_t>(INT32_MAX)) {
            co_return io::io_result{-EOVERFLOW, 0};
        }

        auto* ptr = static_cast<char*>(buffer);
        size_t remaining = length;

        while (remaining > 0) {
            auto result = co_await read(ptr, remaining);
            if (result.result > 0) {
                ptr += result.result;
                remaining -= static_cast<size_t>(result.result);
            } else if (result.result == 0) {
                // Clean EOF before the requested count was satisfied.
                co_return io::io_result{-ENODATA, 0};
            } else if (result.result == -EAGAIN || result.result == -EWOULDBLOCK) {
                // Transient: wait for readability and retry.
                auto poll = co_await poll_read();
                if (poll.result < 0) {
                    co_return poll;
                }
            } else if (result.result == -EINTR) {
                // Interrupted before any data: retry the read directly.
                continue;
            } else {
                // Terminal error.
                co_return result;
            }
        }
        co_return io::io_result{static_cast<int32_t>(length), 0};
    }

    /// Read exactly enough bytes to fill ``buffer``.
    template<typename T>
    coro::task<io::io_result> read_exactly(std::span<T> buffer) {
        return read_exactly(buffer.data(), buffer.size_bytes());
    }

    /// Write exactly ``length`` bytes from ``buffer``.
    ///
    /// Loops over partial writes until ``length`` bytes have been accepted or
    /// a terminal error occurs. A transient readiness error (``-EAGAIN`` /
    /// ``-EWOULDBLOCK``) is not surfaced: the stream waits for the socket to
    /// become writable and retries.
    ///
    /// @return ``io_result`` whose ``result`` is ``length`` on success, or the
    ///         failing ``io_result`` (``result <= 0``) on a terminal error,
    ///         preserving the real error code.
    coro::task<io::io_result> write_exactly(const void* buffer, size_t length) {
        if (length > static_cast<size_t>(INT32_MAX)) {
            co_return io::io_result{-EOVERFLOW, 0};
        }

        const auto* ptr = static_cast<const char*>(buffer);
        size_t remaining = length;

        while (remaining > 0) {
            auto result = co_await write(ptr, remaining);
            if (result.result > 0) {
                ptr += result.result;
                remaining -= static_cast<size_t>(result.result);
            } else if (result.result == -EAGAIN || result.result == -EWOULDBLOCK) {
                // Transient: wait for writability and retry.
                auto poll = co_await poll_write();
                if (poll.result < 0) {
                    co_return poll;
                }
            } else if (result.result == -EINTR) {
                // Interrupted before any bytes were sent: retry the write
                // directly.
                continue;
            } else {
                // Terminal error (or a 0-byte write, which for a socket send
                // indicates the write side can make no further progress).
                co_return result;
            }
        }
        co_return io::io_result{static_cast<int32_t>(length), 0};
    }

    /// Write exactly ``length`` bytes from ``buffer``, cancellable by ``token``.
    coro::task<io::io_result> write_exactly(const void* buffer, size_t length,
                                            coro::cancel_token token) {
        if (length > static_cast<size_t>(INT32_MAX)) {
            co_return io::io_result{-EOVERFLOW, 0};
        }

        const auto* ptr = static_cast<const char*>(buffer);
        size_t remaining = length;

        while (remaining > 0) {
            if (token.is_cancelled()) {
                co_return io::io_result{-ECANCELED, 0};
            }

            auto result = co_await write(ptr, remaining, token);
            if (result.result > 0) {
                ptr += result.result;
                remaining -= static_cast<size_t>(result.result);
            } else {
                co_return result;
            }
        }
        co_return io::io_result{static_cast<int32_t>(length), 0};
    }

    /// Write exactly all bytes from ``buffer``.
    template<typename T>
    coro::task<io::io_result> write_exactly(std::span<const T> buffer) {
        return write_exactly(buffer.data(), buffer.size_bytes());
    }

    /// Write exactly all bytes from ``buffer``, cancellable by ``token``.
    template<typename T>
    coro::task<io::io_result> write_exactly(std::span<const T> buffer,
                                            coro::cancel_token token) {
        return write_exactly(buffer.data(), buffer.size_bytes(), std::move(token));
    }

    /// Write exactly all bytes from ``str``.
    coro::task<io::io_result> write_exactly(std::string_view str) {
        return write_exactly(str.data(), str.size());
    }

    /// Write exactly all bytes from ``str``, cancellable by ``token``.
    coro::task<io::io_result> write_exactly(std::string_view str,
                                            coro::cancel_token token) {
        return write_exactly(str.data(), str.size(), std::move(token));
    }

    /// Async close
    auto close() {
        int fd = fd_;
        fd_ = -1;
        return io::async_close(fd);
    }

    /// Shut down the underlying socket synchronously (kernel-side ::shutdown(2)).
    ///
    /// Safe to call from a different thread than the one driving the stream:
    /// shutdown(2) is thread-safe at the kernel level and is the standard way
    /// to interrupt a pending recv/send on another thread.
    void shutdown_socket() noexcept {
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
        }
    }

    /// Half-close one direction of the connection without releasing the fd.
    /// @param how One of ``SHUT_RD``, ``SHUT_WR``, ``SHUT_RDWR``.
    /// @return true on success, false on error (check ``errno``).
    bool shutdown(int how) noexcept {
        if (fd_ < 0) {
            errno = EBADF;
            return false;
        }
        return ::shutdown(fd_, how) == 0;
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
    /// Release the fd. Prefers ``IORING_OP_CLOSE`` when called from a worker
    /// running on an io_uring backend, so the kernel can drain any in-flight
    /// SQEs on the same fd before the fd table entry is freed for reuse.
    void close_sync() {
        if (fd_ >= 0) {
            io::close_fd_for_destructor(fd_);
            fd_ = -1;
        }
    }

    int fd_ = -1;
    unix_address peer_addr_;
};

/// Unix Domain Socket listener for accepting connections
class uds_listener {
public:
    /// Create and bind a Unix Domain Socket listener
    /// @param addr Address (path) to bind to
    /// @param opts Socket options
    /// @return UDS listener on success; std::nullopt on socket creation
    ///         failure, or on bind/listen error after address conversion
    ///         succeeds (check errno)
    /// @throws std::invalid_argument if addr does not fit in sockaddr_un::sun_path
    static std::optional<uds_listener> bind(
        const unix_address& addr,
        const uds_options& opts = {}) 
    {
        // Validate and materialize the address before creating sockets or
        // unlinking filesystem paths. Overlong UDS addresses throw here.
        auto sa = addr.to_sockaddr();
        socklen_t sa_len = addr.sockaddr_len();

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
            if (::unlink(addr.path.c_str()) != 0 && errno != ENOENT) {
                ELIO_LOG_WARNING("Failed to unlink existing socket path '{}': {} (errno={})",
                              addr.path, strerror(errno), errno);
            }
        }
        
        // Bind
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

        return uds_listener(fd, addr, opts);
    }

    /// Move constructor
    uds_listener(uds_listener&& other) noexcept
        : fd_(other.fd_)
        , local_addr_(std::move(other.local_addr_))
        , opts_(other.opts_) {
        other.fd_ = -1;
    }

    /// Move assignment
    uds_listener& operator=(uds_listener&& other) noexcept {
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
    class accept_awaitable : public io::io_awaitable_base {
    public:
        explicit accept_awaitable(uds_listener& listener)
            : io::io_awaitable_base()
            , listener_(listener) {}

        accept_awaitable(uds_listener& listener, coro::cancel_token token)
            : io::io_awaitable_base()
            , listener_(listener)
            , token_(std::move(token))
            , cancellable_(true) {}

        bool await_ready() const noexcept {
            return false;
        }

        template<typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> awaiter) {
            if (is_cancellable() && token_.is_cancelled()) {
                result_ = io::io_result{-ECANCELED, 0};
                return false;
            }

            bind_to_worker(awaiter);
            auto& ctx = io::current_io_context();

            if (is_cancellable()) {
                auto state = std::make_shared<io::detail::io_cancel_state>();
                state->ctx = &ctx;
                state->awaiter = awaiter;
                state->worker = runtime::worker_thread::current();
                cancel_state_ = state;

                cancel_registration_ = token_.on_cancel([state]() {
                    state->cancelled.store(true, std::memory_order_release);
                    if (!state->worker) {
                        return;
                    }
                    auto exec = io::detail::make_io_cancel_executor(state, true);
                    if (auto* promise = coro::get_promise_base(exec.handle.address())) {
                        promise->set_affinity(state->worker->worker_id());
                        promise->set_worker_local();
                        promise->detach_from_parent();
                    }
                    state->worker->schedule_or_destroy(exec.handle);
                });

                if (token_.is_cancelled()) {
                    cancel_registration_.unregister();
                    state->resumed.store(true, std::memory_order_release);
                    result_ = io::io_result{-ECANCELED, 0};
                    return false;
                }
            }

            io::io_request req{};
            req.op = io::io_op::accept;
            req.fd = listener_.fd_;
            req.addr = reinterpret_cast<struct sockaddr*>(&peer_addr_);
            req.addrlen = &peer_addr_len_;
            req.socket_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
            req.awaiter = awaiter;
            req.state = setup_op_state(awaiter);

            if (cancel_state_) {
                cancel_state_->op = req.state;
                if (cancel_state_->cancelled.load(std::memory_order_acquire)) {
                    clear_op_state();
                    cancel_state_->op = nullptr;
                    cancel_registration_.unregister();
                    result_ = io::io_result{-ECANCELED, 0};
                    return false;
                }
            }

            if (!ctx.prepare(req)) {
                clear_op_state();
                if (cancel_state_) {
                    cancel_state_->op = nullptr;
                }
                cancel_registration_.unregister();
                result_ = io::io_result{-EAGAIN, 0};
                return false;  // Don't suspend, resume immediately
            }
            return true;  // Suspend, will be resumed by completion handler
        }

        std::optional<uds_stream> await_resume() {
            cancel_registration_.unregister();
            if (cancel_state_) {
                cancel_state_->resumed.store(true, std::memory_order_release);
            }
            result_ = read_result_from_op_state();
            restore_affinity();

            if (!result_.success()) {
                errno = result_.error_code();
                return std::nullopt;
            }

            int client_fd = result_.result;
            uds_stream stream(client_fd);

            // Set peer address if available
            if (peer_addr_len_ > offsetof(struct sockaddr_un, sun_path)) {
                stream.set_peer_address(unix_address(peer_addr_));
            }

            ELIO_LOG_DEBUG("Accepted UDS connection");

            return stream;
        }

    private:
        bool is_cancellable() const noexcept { return cancellable_; }

        uds_listener& listener_;
        coro::cancel_token token_;
        coro::cancel_token::registration cancel_registration_;
        std::shared_ptr<io::detail::io_cancel_state> cancel_state_;
        struct sockaddr_un peer_addr_{};
        socklen_t peer_addr_len_ = sizeof(peer_addr_);
        bool cancellable_ = false;
    };
    
    /// Accept a new connection
    auto accept() {
        return accept_awaitable(*this);
    }

    /// Accept a new connection, cancellable by token
    auto accept(coro::cancel_token token) {
        return accept_awaitable(*this, std::move(token));
    }
    
    /// Close the listener
    void close() {
        close_sync();
    }
    
private:
    uds_listener(int fd, const unix_address& addr, const uds_options& opts)
        : fd_(fd), local_addr_(addr), opts_(opts) {}
    
    void close_sync() {
        if (fd_ >= 0) {
            io::close_fd_for_destructor(fd_);
            fd_ = -1;

            // Unlink socket file (only for filesystem sockets). The unlink
            // is a path operation, independent of the fd lifecycle, so it
            // remains synchronous regardless of which close path we took.
            if (!local_addr_.is_abstract() && !local_addr_.path.empty()) {
                ::unlink(local_addr_.path.c_str());
            }

            ELIO_LOG_INFO("UDS listener closed");
        }
    }
    
    int fd_ = -1;
    unix_address local_addr_;
    uds_options opts_;
};

/// Connect to a Unix Domain Socket server
class uds_connect_awaitable : public io::io_awaitable_base {
public:
    uds_connect_awaitable(const unix_address& addr,
                          const uds_options& opts = {})
        : io::io_awaitable_base()
        , addr_(addr), opts_(opts) {}

    ~uds_connect_awaitable() {
        if (fd_ >= 0) {
            io::close_fd_for_destructor(fd_);
            fd_ = -1;
        }
    }

    bool await_ready() const noexcept { return false; }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);

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
            connect_in_progress_ = false;
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
        connect_in_progress_ = true;
        auto& ctx = io::current_io_context();

        io::io_request req{};
        req.op = io::io_op::poll_write;
        req.fd = fd_;
        req.addr = reinterpret_cast<struct sockaddr*>(&sa_);
        req.addrlen = &sa_len_;
        req.awaiter = awaiter;
        req.state = setup_op_state(awaiter);

        if (!ctx.prepare(req)) {
            clear_op_state();
            ::close(fd_);
            fd_ = -1;
            result_ = io::io_result{-EAGAIN, 0};
            return false;  // Don't suspend, resume immediately
        }
        // No explicit submit: poll() auto-submits at the top of its loop
        return true;  // Suspend, will be resumed by completion handler
    }
    
    std::optional<uds_stream> await_resume() {
        restore_affinity();

        // Async path completion result comes from op_state.
        if (connect_in_progress_ && fd_ >= 0) {
            result_ = read_result_from_op_state();
        }

        // For non-blocking connect, writability means completion, not success.
        // Use SO_ERROR to fetch the actual connect result.
        if (connect_in_progress_ && result_.success() && fd_ >= 0) {
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0) {
                result_ = io::io_result{-errno, 0};
            } else if (so_error != 0) {
                result_ = io::io_result{-so_error, 0};
            }
        }

        if (!result_.success()) {
            if (fd_ >= 0) {
                ::close(fd_);
                fd_ = -1;
            }
            errno = result_.error_code();
            return std::nullopt;
        }

        uds_stream stream(fd_);
        fd_ = -1;  // Transfer ownership
        stream.set_peer_address(addr_);

        ELIO_LOG_DEBUG("Connected to {}", addr_.to_string());

        return stream;
    }
    
private:
    unix_address addr_;
    uds_options opts_;
    struct sockaddr_un sa_{};
    socklen_t sa_len_ = 0;
    int fd_ = -1;
    bool connect_in_progress_ = false;
};

/// Connect to a Unix Domain Socket server.
///
/// When awaited, throws std::invalid_argument if addr does not fit in
/// sockaddr_un::sun_path before the connect syscall is attempted. Socket
/// creation/connect failures return std::nullopt and set errno.
inline auto uds_connect(const unix_address& addr,
                        const uds_options& opts = {}) {
    return uds_connect_awaitable(addr, opts);
}

/// Connect to a Unix Domain Socket server by path.
///
/// When awaited, throws std::invalid_argument if path does not fit in
/// sockaddr_un::sun_path before the connect syscall is attempted. Socket
/// creation/connect failures return std::nullopt and set errno.
inline auto uds_connect(std::string_view path,
                        const uds_options& opts = {}) {
    return uds_connect_awaitable(unix_address(path), opts);
}

} // namespace elio::net
