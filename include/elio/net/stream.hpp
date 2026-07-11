#pragma once

/// @file stream.hpp
/// @brief Stream abstraction for TCP connections, with optional TLS support
///
/// This file provides a polymorphic stream wrapper that abstracts over
/// plain TCP streams and, when ELIO_HAS_TLS is enabled, TLS-encrypted streams,
/// eliminating code duplication in HTTP, WebSocket, and SSE clients.

#include <elio/net/tcp.hpp>
#include <elio/net/resolve.hpp>
#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
#include <elio/tls/tls_context.hpp>
#include <elio/tls/tls_stream.hpp>
#endif
#include <elio/io/io_context.hpp>
#include <elio/coro/task.hpp>

#include <variant>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>

namespace elio::net {

/// Stream type that can be plain TCP, or TLS encrypted when ELIO_HAS_TLS is set.
/// Provides a common interface for read/write operations.
class stream {
public:
#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
    using variant_type = std::variant<std::monostate, tcp_stream, tls::tls_stream>;
#else
    using variant_type = std::variant<std::monostate, tcp_stream>;
#endif

    /// Default constructor - creates a disconnected stream
    stream() = default;

    /// Create from a plain TCP stream
    explicit stream(tcp_stream tcp)
        : stream_(std::move(tcp)) {}

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
    /// Create from a TLS stream
    explicit stream(tls::tls_stream tls)
        : stream_(std::move(tls)) {}
#endif

    // Move only
    stream(stream&&) = default;
    stream& operator=(stream&&) = default;
    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    /// Check if connected (not in monostate)
    bool is_connected() const noexcept {
        return !std::holds_alternative<std::monostate>(stream_);
    }

    /// Check if this is a secure (TLS) connection
    bool is_secure() const noexcept { return is_tls(); }

    /// Read data from the stream
    coro::task<io::io_result> read(void* buffer, size_t length) {
        if (auto* tcp = std::get_if<tcp_stream>(&stream_)) {
            co_return co_await tcp->read(buffer, length);
        }
#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
        if (auto* tls = std::get_if<tls::tls_stream>(&stream_)) {
            co_return co_await tls->read(buffer, length);
        }
#endif
        co_return io::io_result{-ENOTCONN, 0};
    }

    /// Write data to the stream
    coro::task<io::io_result> write(const void* buffer, size_t length) {
        if (auto* tcp = std::get_if<tcp_stream>(&stream_)) {
            co_return co_await tcp->write(buffer, length);
        }
#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
        if (auto* tls = std::get_if<tls::tls_stream>(&stream_)) {
            co_return co_await tls->write(buffer, length);
        }
#endif
        co_return io::io_result{-ENOTCONN, 0};
    }

    /// Write string_view data
    coro::task<io::io_result> write(std::string_view data) {
        return write(data.data(), data.size());
    }

    /// Read exactly ``length`` bytes into ``buffer``.
    ///
    /// Loops over partial reads until ``length`` bytes have been stored, a
    /// terminal error occurs, or the peer closes the connection. Delegates to
    /// the active underlying stream's ``read_exactly``.
    ///
    /// @return io_result with ``result == length`` on success. Returns
    ///         ``-ENODATA`` if the peer closes before ``length`` bytes arrive,
    ///         ``-ENOTCONN`` if the stream is disconnected, or the underlying
    ///         terminal error otherwise.
    coro::task<io::io_result> read_exactly(void* buffer, size_t length) {
        if (auto* tcp = std::get_if<tcp_stream>(&stream_)) {
            co_return co_await tcp->read_exactly(buffer, length);
        }
#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
        if (auto* tls = std::get_if<tls::tls_stream>(&stream_)) {
            co_return co_await tls->read_exactly(buffer, length);
        }
#endif
        co_return io::io_result{-ENOTCONN, 0};
    }

    /// Write exactly ``length`` bytes from ``buffer``, retrying short writes.
    ///
    /// Delegates to the active underlying stream's ``write_exactly``.
    ///
    /// @return io_result with ``result == length`` on success, ``-ENOTCONN`` if
    ///         disconnected, or the failing io_result (``result <= 0``) on a
    ///         terminal error, preserving the real error code.
    coro::task<io::io_result> write_exactly(const void* buffer, size_t length) {
        if (auto* tcp = std::get_if<tcp_stream>(&stream_)) {
            co_return co_await tcp->write_exactly(buffer, length);
        }
#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
        if (auto* tls = std::get_if<tls::tls_stream>(&stream_)) {
            co_return co_await tls->write_exactly(buffer, length);
        }
#endif
        co_return io::io_result{-ENOTCONN, 0};
    }

    /// Write exactly all bytes from ``data``.
    coro::task<io::io_result> write_exactly(std::string_view data) {
        return write_exactly(data.data(), data.size());
    }

    /// Write all data, retrying short writes.
    /// @deprecated Prefer ``write_exactly``; retained as a compatibility alias.
    /// @return io_result with total bytes written on success, or the failing
    ///         io_result (result < 0) on a hard error, preserving the real
    ///         error code
    ///
    /// Transient readiness results (-EAGAIN/-EWOULDBLOCK) and interruptions
    /// (-EINTR) are retried rather than treated as terminal: the underlying
    /// stream write() awaits writability before retrying, so this does not
    /// busy-loop. A zero-byte write terminates the loop to avoid spinning.
    coro::task<io::io_result> write_all(const void* buffer, size_t length) {
        return write_exactly(buffer, length);
    }

    /// Write all data from string_view.
    /// @deprecated Prefer ``write_exactly``; retained as a compatibility alias.
    coro::task<io::io_result> write_all(std::string_view data) {
        return write_exactly(data.data(), data.size());
    }

    /// Close/shutdown the stream
    coro::task<void> close() {
#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
        if (auto* tls = std::get_if<tls::tls_stream>(&stream_)) {
            co_await tls->shutdown();
            stream_ = std::monostate{};
            co_return;
        }
#endif
        if (auto* tcp = std::get_if<tcp_stream>(&stream_)) {
            // Plain TCP streams have an async close() method
            co_await tcp->close();
        }
        stream_ = std::monostate{};
    }

    /// Disconnect without TLS shutdown (synchronous)
    /// Use this when you need to reset the stream without an async operation
    void disconnect() noexcept {
        stream_ = std::monostate{};
    }

    /// Mark the active TLS stream as externally shut down.
    ///
    /// Timeout watchdogs may interrupt an in-flight TLS read/write by calling
    /// shutdown(2) on the file descriptor from another coroutine.  After the
    /// I/O operation returns and the watchdog has been joined, call this before
    /// destroying the stream so tls_stream skips SSL_shutdown on the unusable
    /// socket. Plain TCP streams do not need any extra bookkeeping.
    void mark_externally_shut_down() noexcept {
#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
        if (auto* tls = std::get_if<tls::tls_stream>(&stream_)) {
            tls->mark_externally_shut_down();
        }
#endif
    }

    /// Get last use time (for connection pooling)
    std::chrono::steady_clock::time_point last_use() const noexcept {
        return last_use_;
    }

    /// Update last use time
    void touch() {
        last_use_ = std::chrono::steady_clock::now();
    }

    /// Get underlying file descriptor (-1 if not connected)
    int fd() const noexcept {
        return std::visit([](const auto& s) -> int {
            if constexpr (std::is_same_v<std::decay_t<decltype(s)>, std::monostate>) {
                return -1;
            } else {
                return s.fd();
            }
        }, stream_);
    }

    /// Access underlying TCP stream (throws if not TCP or disconnected)
    tcp_stream& as_tcp() {
        return std::get<tcp_stream>(stream_);
    }

    const tcp_stream& as_tcp() const {
        return std::get<tcp_stream>(stream_);
    }

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
    /// Access underlying TLS stream (throws if not TLS or disconnected)
    tls::tls_stream& as_tls() {
        return std::get<tls::tls_stream>(stream_);
    }

    const tls::tls_stream& as_tls() const {
        return std::get<tls::tls_stream>(stream_);
    }
#endif

    /// Check if holds TCP stream
    bool is_tcp() const noexcept {
        return std::holds_alternative<tcp_stream>(stream_);
    }

    /// Check if holds TLS stream
    bool is_tls() const noexcept {
#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
        return std::holds_alternative<tls::tls_stream>(stream_);
#else
        return false;
#endif
    }

private:
    variant_type stream_;
    std::chrono::steady_clock::time_point last_use_ = std::chrono::steady_clock::now();
};

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
/// Connect to a host, automatically selecting TCP or TLS based on secure flag.
/// @param host Hostname to connect to.
/// @param port Port number.
/// @param secure If true, establish TLS connection.
/// @param tls_ctx TLS context (required if secure=true).
/// @return Connected stream on success, std::nullopt on error.
inline coro::task<std::optional<stream>>
connect(std::string_view host, uint16_t port, bool secure = false,
        tls::tls_context* tls_ctx = nullptr,
        resolve_options resolve_opts = default_cached_resolve_options()) {
    if (secure) {
        if (!tls_ctx) {
            co_return std::nullopt;
        }
        auto result = co_await tls::tls_connect(*tls_ctx, host, port, resolve_opts);
        if (!result) {
            co_return std::nullopt;
        }
        co_return stream(std::move(*result));
    } else {
        auto resolved = co_await resolve_hostname(host, port, resolve_opts);
        if (!resolved) {
            co_return std::nullopt;
        }

        auto result = co_await tcp_connect(*resolved);
        if (!result) {
            co_return std::nullopt;
        }
        co_return stream(std::move(*result));
    }
}
#else
/// Connect to a host using plain TCP. If secure is requested without TLS
/// support compiled in, the connection fails with std::nullopt.
inline coro::task<std::optional<stream>>
connect(std::string_view host, uint16_t port, bool secure = false,
        std::nullptr_t tls_ctx = nullptr,
        resolve_options resolve_opts = default_cached_resolve_options()) {
    (void)tls_ctx;
    if (secure) {
        errno = ENOTSUP;
        co_return std::nullopt;
    }

    auto resolved = co_await resolve_hostname(host, port, resolve_opts);
    if (!resolved) {
        co_return std::nullopt;
    }

    auto result = co_await tcp_connect(*resolved);
    if (!result) {
        co_return std::nullopt;
    }
    co_return stream(std::move(*result));
}
#endif

} // namespace elio::net
