#pragma once

#include <elio/tls/tls_context.hpp>
#include <elio/net/tcp.hpp>
#include <elio/net/resolve.hpp>
#include <elio/io/io_context.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/coro/task.hpp>
#include <elio/time/timer.hpp>
#include <elio/log/macros.hpp>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <sys/socket.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <memory>
#include <optional>
#include <span>

namespace elio::tls {

/// TLS handshake result
enum class handshake_result {
    success,
    want_read,
    want_write,
    error
};

/// TLS stream wrapping a TCP connection with SSL/TLS encryption
///
/// **Thread safety:** like the underlying ``SSL*``, a ``tls_stream`` is **not**
/// safe for concurrent use from multiple coroutines/threads. Issuing
/// ``read``/``write``/``shutdown`` on the same instance from different
/// coroutines simultaneously is undefined behavior — OpenSSL does not protect
/// SSL state against concurrent operations. Callers that need concurrent
/// writes must serialize them externally (for example with a ``sync::mutex``,
/// as done in the WebSocket layer's ``send_mutex_``).
class tls_stream {
public:
    /// Create a TLS stream from an existing TCP stream
    /// @param tcp The underlying TCP stream (takes ownership)
    /// @param ctx TLS context to use
    tls_stream(net::tcp_stream tcp, tls_context& ctx)
        : tcp_(std::move(tcp)) {
        ssl_ = SSL_new(ctx.native_handle());
        if (!ssl_) {
            throw std::runtime_error("Failed to create SSL object");
        }
        
        // Set the file descriptor
        SSL_set_fd(ssl_, tcp_.fd());
        
        // Set mode based on context
        if (ctx.mode() == tls_mode::client) {
            SSL_set_connect_state(ssl_);
        } else {
            SSL_set_accept_state(ssl_);
        }
        
        mode_ = ctx.mode();
    }
    
    /// Destructor
    ~tls_stream() {
        if (ssl_) {
            // If the user forgot to ``co_await stream.shutdown()``, at least
            // queue our close_notify alert so the peer can distinguish a
            // clean close from a MITM truncation. The underlying fd is
            // non-blocking, so a single ``SSL_shutdown`` call is bounded:
            // it serializes the alert and tries to write it; if the kernel
            // buffer is full we accept the loss rather than block here.
            // We never wait for the peer's close_notify in the destructor,
            // since that would require async I/O.
            //
            // Skip SSL_shutdown when the underlying socket is already known
            // to be unusable. SSL_shutdown's BIO writes via send(2); on a
            // half-closed socket that produces EPIPE and, on OpenSSL builds
            // that don't set MSG_NOSIGNAL (older OpenSSL, musl, etc.),
            // delivers SIGPIPE to the process. We avoid the write entirely
            // when (a) somebody else (e.g. a slow-loris watchdog) called
            // ``shutdown_socket()`` / ``mark_externally_shut_down()`` on us,
            // or (b) the kernel reports a pending socket error.
            if (handshake_complete_ && !shutdown_sent_ &&
                !is_socket_closed_or_dead()) {
                ERR_clear_error();
                (void)SSL_shutdown(ssl_);
            }
            SSL_free(ssl_);
        }
    }

    // Non-copyable
    tls_stream(const tls_stream&) = delete;
    tls_stream& operator=(const tls_stream&) = delete;

    // Movable
    tls_stream(tls_stream&& other) noexcept
        : tcp_(std::move(other.tcp_))
        , ssl_(other.ssl_)
        , mode_(other.mode_)
        , handshake_complete_(other.handshake_complete_)
        , shutdown_sent_(other.shutdown_sent_)
        , externally_shut_down_(
              other.externally_shut_down_.load(std::memory_order_acquire)) {
        other.ssl_ = nullptr;
        other.handshake_complete_ = false;
        other.shutdown_sent_ = false;
        other.externally_shut_down_.store(false, std::memory_order_release);
    }

    tls_stream& operator=(tls_stream&& other) noexcept {
        if (this != &other) {
            if (ssl_) {
                if (handshake_complete_ && !shutdown_sent_ &&
                    !is_socket_closed_or_dead()) {
                    ERR_clear_error();
                    (void)SSL_shutdown(ssl_);
                }
                SSL_free(ssl_);
            }
            tcp_ = std::move(other.tcp_);
            ssl_ = other.ssl_;
            mode_ = other.mode_;
            handshake_complete_ = other.handshake_complete_;
            shutdown_sent_ = other.shutdown_sent_;
            externally_shut_down_.store(
                other.externally_shut_down_.load(std::memory_order_acquire),
                std::memory_order_release);
            other.ssl_ = nullptr;
            other.handshake_complete_ = false;
            other.shutdown_sent_ = false;
            other.externally_shut_down_.store(false, std::memory_order_release);
        }
        return *this;
    }
    
    /// Set SNI hostname (for client connections)
    void set_hostname(std::string_view hostname) {
        hostname_ = std::string(hostname);
        // Set SNI extension
        SSL_set_tlsext_host_name(ssl_, hostname_.c_str());
        
        // Configure hostname verification for OpenSSL 1.1.0+
        X509_VERIFY_PARAM* param = SSL_get0_param(ssl_);
        X509_VERIFY_PARAM_set1_host(param, hostname_.c_str(), hostname_.size());
    }
    
    /// Perform TLS handshake asynchronously
    /// @return true on success, false on error (check errno)
    coro::task<bool> handshake() {
        while (true) {
            ERR_clear_error();
            int ret = (mode_ == tls_mode::client) ? SSL_connect(ssl_) : SSL_accept(ssl_);
            
            if (ret == 1) {
                handshake_complete_ = true;
                ELIO_LOG_DEBUG("TLS handshake complete (protocol: {}, cipher: {})",
                              SSL_get_version(ssl_), SSL_get_cipher_name(ssl_));
                co_return true;
            }
            
            int err = SSL_get_error(ssl_, ret);
            
            if (err == SSL_ERROR_WANT_READ) {
                auto result = co_await tcp_.poll_read();
                if (result.result < 0) {
                    errno = -result.result;
                    co_return false;
                }
            } else if (err == SSL_ERROR_WANT_WRITE) {
                auto result = co_await tcp_.poll_write();
                if (result.result < 0) {
                    errno = -result.result;
                    co_return false;
                }
            } else {
                // Handshake failed
                long verify_err = SSL_get_verify_result(ssl_);
                if (verify_err != X509_V_OK) {
                    ELIO_LOG_ERROR("TLS certificate verification failed: {} ({})", 
                                  X509_verify_cert_error_string(verify_err), verify_err);
                }
                ELIO_LOG_ERROR("TLS handshake failed: {}", get_ssl_error_string(err));
                errno = err;
                co_return false;
            }
        }
    }
    
    /// Read data asynchronously
    /// @param buffer Buffer to read into
    /// @param length Maximum bytes to read
    /// @return Number of bytes read, or negative error code
    coro::task<io::io_result> read(void* buffer, size_t length) {
        if (!handshake_complete_) {
            auto hs = co_await handshake();
            if (!hs) {
                co_return io::io_result{-errno, 0};
            }
        }
        
        while (true) {
            ERR_clear_error();
            int ret = SSL_read(ssl_, buffer, static_cast<int>(length));
            
            if (ret > 0) {
                co_return io::io_result{ret, 0};
            }
            
            if (ret == 0) {
                // Connection closed
                int err = SSL_get_error(ssl_, ret);
                if (err == SSL_ERROR_ZERO_RETURN) {
                    // Clean shutdown
                    co_return io::io_result{0, 0};
                }
                co_return io::io_result{-EIO, 0};
            }
            
            int err = SSL_get_error(ssl_, ret);
            
            if (err == SSL_ERROR_WANT_READ) {
                auto result = co_await tcp_.poll_read();
                if (result.result < 0) {
                    co_return result;
                }
            } else if (err == SSL_ERROR_WANT_WRITE) {
                auto result = co_await tcp_.poll_write();
                if (result.result < 0) {
                    co_return result;
                }
            } else {
                ELIO_LOG_ERROR("TLS read error: {}", get_ssl_error_string(err));
                co_return io::io_result{-EIO, 0};
            }
        }
    }
    
    /// Write data asynchronously
    /// @param buffer Data to write
    /// @param length Number of bytes to write
    /// @return Number of bytes written, or negative error code
    coro::task<io::io_result> write(const void* buffer, size_t length) {
        if (!handshake_complete_) {
            auto hs = co_await handshake();
            if (!hs) {
                co_return io::io_result{-errno, 0};
            }
        }
        
        while (true) {
            ERR_clear_error();
            int ret = SSL_write(ssl_, buffer, static_cast<int>(length));
            
            if (ret > 0) {
                co_return io::io_result{ret, 0};
            }
            
            int err = SSL_get_error(ssl_, ret);
            
            if (err == SSL_ERROR_WANT_READ) {
                auto result = co_await tcp_.poll_read();
                if (result.result < 0) {
                    co_return result;
                }
            } else if (err == SSL_ERROR_WANT_WRITE) {
                auto result = co_await tcp_.poll_write();
                if (result.result < 0) {
                    co_return result;
                }
            } else {
                ELIO_LOG_ERROR("TLS write error: {}", get_ssl_error_string(err));
                co_return io::io_result{-EIO, 0};
            }
        }
    }
    
    /// Write string data
    coro::task<io::io_result> write(std::string_view data) {
        return write(data.data(), data.size());
    }

    /// Read exactly ``length`` bytes into ``buffer``.
    ///
    /// Loops over partial reads until ``length`` bytes have been stored, a
    /// terminal error occurs, or the peer closes the connection (EOF). The
    /// underlying TLS ``read`` already waits for readiness internally, so no
    /// ``-EAGAIN`` / ``-EWOULDBLOCK`` is ever surfaced here.
    ///
    /// @return ``io_result`` whose ``result`` is ``length`` on success. If the
    ///         peer closes before ``length`` bytes arrive, returns ``-ENODATA``
    ///         (short read / unexpected EOF). Any other terminal error from the
    ///         underlying ``read`` is returned as-is.
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
            } else {
                // Terminal error (TLS read never returns -EAGAIN).
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
    /// Loops over partial writes until ``length`` bytes have been accepted or a
    /// terminal error occurs. The underlying TLS ``write`` already waits for
    /// readiness internally, so no ``-EAGAIN`` / ``-EWOULDBLOCK`` is ever
    /// surfaced here.
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
            } else {
                // Terminal error (TLS write never returns -EAGAIN).
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

    /// Write exactly all bytes from ``str``.
    coro::task<io::io_result> write_exactly(std::string_view str) {
        return write_exactly(str.data(), str.size());
    }

    /// Perform TLS shutdown.
    ///
    /// Splits the handshake into two phases:
    ///   1. Send our ``close_notify`` (loop on WANT_WRITE).
    ///   2. Wait for the peer's ``close_notify`` (loop on WANT_READ).
    ///
    /// A wall-clock budget bounds the wait so a misbehaving peer cannot stall
    /// the caller indefinitely. The default budget of 2 seconds matches the
    /// guidance in TLS implementations; pass a longer duration if needed.
    coro::task<void> shutdown(std::chrono::milliseconds timeout =
                                  std::chrono::milliseconds(2000)) {
        if (!ssl_ || !handshake_complete_) {
            co_return;
        }

        using namespace std::chrono_literals;

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        auto remaining = [&]() {
            return std::chrono::steady_clock::now() < deadline;
        };
        auto poll_until_deadline = [&](bool for_read) -> coro::task<io::io_result> {
            if (!remaining()) {
                co_return io::io_result{-ETIMEDOUT, 0};
            }

            coro::cancel_source poll_cancel;
            auto watchdog = elio::spawn([deadline, &poll_cancel]() -> coro::task<void> {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    poll_cancel.cancel();
                    co_return;
                }

                auto result = co_await time::sleep_for(
                    deadline - now, poll_cancel.get_token());
                if (result == coro::cancel_result::completed) {
                    poll_cancel.cancel();
                }
            });

            io::cancellable_io_result poll{};
            if (for_read) {
                poll = co_await tcp_.poll_read(poll_cancel.get_token());
            } else {
                poll = co_await tcp_.poll_write(poll_cancel.get_token());
            }

            poll_cancel.cancel();
            co_await watchdog;

            if (poll.was_cancelled() || poll.io.result == -ECANCELED) {
                co_return io::io_result{-ETIMEDOUT, 0};
            }
            co_return poll.io;
        };

        // Phase 1: emit our close_notify. ``SSL_shutdown`` returning 0 means
        // our alert has been queued/sent (peer's not yet observed); 1 means
        // both sides have already exchanged close_notify. WANT_WRITE means
        // the underlying socket can't accept more data right now — poll and
        // retry. WANT_READ at this stage is unusual but possible when there
        // is still inbound data buffered to drain.
        while (remaining()) {
            ERR_clear_error();
            int ret = SSL_shutdown(ssl_);
            if (ret >= 0) {
                shutdown_sent_ = true;
                if (ret == 1) {
                    handshake_complete_ = false;
                    co_return;
                }
                break;  // ret == 0: our close_notify is out, fall through to phase 2
            }
            int err = SSL_get_error(ssl_, ret);
            if (err == SSL_ERROR_WANT_WRITE) {
                auto poll = co_await poll_until_deadline(false);
                if (poll.result < 0) {
                    handshake_complete_ = false;
                    co_return;
                }
            } else if (err == SSL_ERROR_WANT_READ) {
                auto poll = co_await poll_until_deadline(true);
                if (poll.result < 0) {
                    handshake_complete_ = false;
                    co_return;
                }
            } else {
                // Hard error — abandon shutdown.
                handshake_complete_ = false;
                co_return;
            }
        }

        if (!shutdown_sent_) {
            // Timed out before our alert went out.
            handshake_complete_ = false;
            co_return;
        }

        // Phase 2: wait for peer's close_notify. Each non-progress loop
        // iteration must consume time from the wall-clock budget so a
        // chatty peer that keeps producing WANT_READ/WANT_WRITE cycles
        // can't keep us here forever.
        while (remaining()) {
            ERR_clear_error();
            int ret = SSL_shutdown(ssl_);
            if (ret == 1) {
                break;
            }
            if (ret == 0) {
                // Our close_notify is out, peer's hasn't arrived yet.
                auto poll = co_await poll_until_deadline(true);
                if (poll.result < 0) break;
                continue;
            }
            int err = SSL_get_error(ssl_, ret);
            if (err == SSL_ERROR_WANT_READ) {
                auto poll = co_await poll_until_deadline(true);
                if (poll.result < 0) break;
            } else if (err == SSL_ERROR_WANT_WRITE) {
                auto poll = co_await poll_until_deadline(false);
                if (poll.result < 0) break;
            } else {
                break;
            }
        }

        handshake_complete_ = false;
    }
    
    /// Get negotiated ALPN protocol
    std::string_view alpn_protocol() const {
        const unsigned char* proto = nullptr;
        unsigned int len = 0;
        SSL_get0_alpn_selected(ssl_, &proto, &len);
        if (proto && len > 0) {
            return std::string_view(reinterpret_cast<const char*>(proto), len);
        }
        return {};
    }
    
    /// Get TLS version string
    const char* version() const {
        return SSL_get_version(ssl_);
    }
    
    /// Get cipher name
    const char* cipher() const {
        return SSL_get_cipher_name(ssl_);
    }
    
    /// Get underlying file descriptor
    int fd() const noexcept { return tcp_.fd(); }

    /// Get underlying TCP stream (const)
    const net::tcp_stream& tcp() const noexcept { return tcp_; }

    /// Check if handshake is complete
    bool is_handshake_complete() const noexcept { return handshake_complete_; }

    /// Mark this stream as having had its socket shut down externally
    /// (e.g. by a slow-loris watchdog running on a different thread).
    ///
    /// After this call the destructor will skip ``SSL_shutdown`` because the
    /// underlying socket can no longer accept the close_notify write; on
    /// OpenSSL versions / libc builds that don't set ``MSG_NOSIGNAL`` the
    /// write would deliver SIGPIPE to the process.
    ///
    /// This method only flips a flag and is safe to call from any thread;
    /// it must, however, happen-before the destructor runs (typically by
    /// joining the watchdog coroutine before letting the stream go out of
    /// scope).
    void mark_externally_shut_down() noexcept {
        externally_shut_down_.store(true, std::memory_order_release);
    }

    /// Convenience: kernel-side ``::shutdown(fd, SHUT_RDWR)`` plus
    /// ``mark_externally_shut_down()``. Intended for watchdog code that
    /// needs to interrupt a pending recv on a different thread.
    void shutdown_socket() noexcept {
        mark_externally_shut_down();
        if (int fd = tcp_.fd(); fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
        }
    }
    
    /// Get peer certificate (if any)
    X509* peer_certificate() const {
        return SSL_get_peer_certificate(ssl_);
    }
    
    /// Verify peer certificate result
    long verify_result() const {
        return SSL_get_verify_result(ssl_);
    }
    
private:
    /// Cheap probe that returns true when the underlying socket is either
    /// already known (via the externally_shut_down_ flag) to be unusable,
    /// or when the kernel reports a pending error such as ECONNRESET/EPIPE.
    /// Used by the destructor to decide whether SSL_shutdown's close_notify
    /// write would be wasted (and potentially fatal via SIGPIPE).
    bool is_socket_closed_or_dead() const noexcept {
        if (externally_shut_down_.load(std::memory_order_acquire)) {
            return true;
        }
        int fd = tcp_.fd();
        if (fd < 0) {
            return true;
        }
        int sock_err = 0;
        socklen_t len = sizeof(sock_err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &len) == 0
            && sock_err != 0) {
            return true;
        }
        return false;
    }

    static std::string get_ssl_error_string(int err) {
        switch (err) {
            case SSL_ERROR_NONE: return "none";
            case SSL_ERROR_SSL: {
                char buf[256];
                ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
                return buf;
            }
            case SSL_ERROR_WANT_READ: return "want_read";
            case SSL_ERROR_WANT_WRITE: return "want_write";
            case SSL_ERROR_WANT_X509_LOOKUP: return "want_x509_lookup";
            case SSL_ERROR_SYSCALL: return "syscall error: " + std::string(strerror(errno));
            case SSL_ERROR_ZERO_RETURN: return "zero_return";
            case SSL_ERROR_WANT_CONNECT: return "want_connect";
            case SSL_ERROR_WANT_ACCEPT: return "want_accept";
            default: return "unknown(" + std::to_string(err) + ")";
        }
    }
    
    net::tcp_stream tcp_;
    SSL* ssl_ = nullptr;
    tls_mode mode_ = tls_mode::client;
    bool handshake_complete_ = false;
    bool shutdown_sent_ = false;  ///< True once our close_notify has been queued.
    /// Set by ``mark_externally_shut_down()`` / ``shutdown_socket()`` when a
    /// foreign actor (typically a watchdog on another thread) has already
    /// shut down the underlying TCP socket. The destructor reads this to
    /// avoid an SSL_shutdown() that would write to a half-closed socket.
    std::atomic<bool> externally_shut_down_{false};
    std::string hostname_;  // Store hostname for SNI and verification
};

/// Connect to a TLS server
/// @param ctx TLS context (client mode)
/// @param host Hostname to connect to
/// @param port Port to connect to
/// @return TLS stream on success, std::nullopt on error (check errno)
inline coro::task<std::optional<tls_stream>> 
tls_connect(tls_context& ctx,
            std::string_view host,
            uint16_t port,
            net::resolve_options resolve_opts = net::default_cached_resolve_options()) {
    auto resolved = co_await net::resolve_all(host, port, resolve_opts);
    if (resolved.empty()) {
        co_return std::nullopt;
    }

    for (const auto& addr : resolved) {
        auto tcp_result = co_await net::tcp_connect(addr);
        if (!tcp_result) {
            continue;
        }

        tls_stream stream(std::move(*tcp_result), ctx);
        stream.set_hostname(host);

        auto hs_result = co_await stream.handshake();
        if (!hs_result) {
            continue;
        }

        co_return std::move(stream);
    }

    co_return std::nullopt;
}

/// TLS listener for accepting secure connections
class tls_listener {
public:
    /// Create a TLS listener from a TCP listener and TLS context
    tls_listener(net::tcp_listener tcp, tls_context& ctx)
        : tcp_(std::move(tcp)), ctx_(&ctx) {}
    
    /// Accept a new TLS connection
    /// @return TLS stream on success, std::nullopt on error (check errno)
    coro::task<std::optional<tls_stream>> accept() {
        auto tcp_result = co_await tcp_.accept();
        if (!tcp_result) {
            co_return std::nullopt;
        }
        
        tls_stream stream(std::move(*tcp_result), *ctx_);
        
        // Perform handshake
        auto hs_result = co_await stream.handshake();
        if (!hs_result) {
            co_return std::nullopt;
        }
        
        co_return std::move(stream);
    }
    
    /// Get underlying file descriptor
    int fd() const noexcept { return tcp_.fd(); }
    
    /// Bind and create a TLS listener (IPv4)
    /// @return TLS listener on success, std::nullopt on error (check errno)
    static std::optional<tls_listener>
    bind(const net::ipv4_address& addr, tls_context& ctx) {
        auto tcp_result = net::tcp_listener::bind(addr);
        if (!tcp_result) {
            return std::nullopt;
        }
        return tls_listener(std::move(*tcp_result), ctx);
    }

    /// Bind and create a TLS listener (IPv6)
    static std::optional<tls_listener>
    bind(const net::ipv6_address& addr, tls_context& ctx) {
        auto tcp_result = net::tcp_listener::bind(addr);
        if (!tcp_result) {
            return std::nullopt;
        }
        return tls_listener(std::move(*tcp_result), ctx);
    }

    /// Bind and create a TLS listener (generic address)
    static std::optional<tls_listener>
    bind(const net::socket_address& addr, tls_context& ctx) {
        auto tcp_result = net::tcp_listener::bind(addr);
        if (!tcp_result) {
            return std::nullopt;
        }
        return tls_listener(std::move(*tcp_result), ctx);
    }
    
private:
    net::tcp_listener tcp_;
    tls_context* ctx_;
};

} // namespace elio::tls
