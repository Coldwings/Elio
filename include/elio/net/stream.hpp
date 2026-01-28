#pragma once

/// @file stream.hpp
/// @brief Unified stream abstraction for TCP and TLS connections
///
/// This file provides a polymorphic stream wrapper that abstracts over
/// plain TCP streams and TLS-encrypted streams, eliminating code
/// duplication in HTTP, WebSocket, and SSE clients.

#include <elio/net/tcp.hpp>
#include <elio/tls/tls_context.hpp>
#include <elio/tls/tls_stream.hpp>
#include <elio/io/io_context.hpp>
#include <elio/coro/task.hpp>

#include <variant>
#include <chrono>

namespace elio::net {

/// Unified stream type that can be either plain TCP or TLS encrypted
/// Provides a common interface for read/write operations
class stream {
public:
    using variant_type = std::variant<std::monostate, tcp_stream, tls::tls_stream>;

    /// Default constructor - creates a disconnected stream
    stream() = default;

    /// Create from a plain TCP stream
    explicit stream(tcp_stream tcp)
        : stream_(std::move(tcp)), secure_(false) {}

    /// Create from a TLS stream
    explicit stream(tls::tls_stream tls)
        : stream_(std::move(tls)), secure_(true) {}

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
    bool is_secure() const noexcept { return secure_; }

    /// Read data from the stream
    /// @param buffer Buffer to read into
    /// @param length Maximum bytes to read
    /// @return io_result with bytes read or error
    coro::task<io::io_result> read(void* buffer, size_t length) {
        if (std::holds_alternative<tcp_stream>(stream_)) {
            co_return co_await std::get<tcp_stream>(stream_).read(buffer, length);
        } else if (std::holds_alternative<tls::tls_stream>(stream_)) {
            co_return co_await std::get<tls::tls_stream>(stream_).read(buffer, length);
        }
        co_return io::io_result{-ENOTCONN, 0};
    }

    /// Write data to the stream
    /// @param buffer Data to write
    /// @param length Number of bytes to write
    /// @return io_result with bytes written or error
    coro::task<io::io_result> write(const void* buffer, size_t length) {
        if (std::holds_alternative<tcp_stream>(stream_)) {
            co_return co_await std::get<tcp_stream>(stream_).write(buffer, length);
        } else if (std::holds_alternative<tls::tls_stream>(stream_)) {
            co_return co_await std::get<tls::tls_stream>(stream_).write(buffer, length);
        }
        co_return io::io_result{-ENOTCONN, 0};
    }

    /// Write string_view data
    coro::task<io::io_result> write(std::string_view data) {
        return write(data.data(), data.size());
    }

    /// Write all data, retrying short writes
    /// @return true if all data was written, false on error
    coro::task<bool> write_all(const void* buffer, size_t length) {
        const auto* ptr = static_cast<const char*>(buffer);
        size_t remaining = length;

        while (remaining > 0) {
            auto result = co_await write(ptr, remaining);
            if (result.result <= 0) {
                co_return false;
            }
            ptr += result.result;
            remaining -= static_cast<size_t>(result.result);
        }
        co_return true;
    }

    /// Write all data from string_view
    coro::task<bool> write_all(std::string_view data) {
        return write_all(data.data(), data.size());
    }

    /// Close/shutdown the stream
    coro::task<void> close() {
        if (std::holds_alternative<tls::tls_stream>(stream_)) {
            co_await std::get<tls::tls_stream>(stream_).shutdown();
        }
        stream_ = std::monostate{};
    }

    /// Disconnect without TLS shutdown (synchronous)
    /// Use this when you need to reset the stream without an async operation
    void disconnect() noexcept {
        stream_ = std::monostate{};
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
        if (std::holds_alternative<tcp_stream>(stream_)) {
            return std::get<tcp_stream>(stream_).fd();
        } else if (std::holds_alternative<tls::tls_stream>(stream_)) {
            return std::get<tls::tls_stream>(stream_).fd();
        }
        return -1;
    }

    /// Access underlying TCP stream (throws if not TCP or disconnected)
    tcp_stream& as_tcp() {
        return std::get<tcp_stream>(stream_);
    }

    const tcp_stream& as_tcp() const {
        return std::get<tcp_stream>(stream_);
    }

    /// Access underlying TLS stream (throws if not TLS or disconnected)
    tls::tls_stream& as_tls() {
        return std::get<tls::tls_stream>(stream_);
    }

    const tls::tls_stream& as_tls() const {
        return std::get<tls::tls_stream>(stream_);
    }

    /// Check if holds TCP stream
    bool is_tcp() const noexcept {
        return std::holds_alternative<tcp_stream>(stream_);
    }

    /// Check if holds TLS stream
    bool is_tls() const noexcept {
        return std::holds_alternative<tls::tls_stream>(stream_);
    }

private:
    variant_type stream_;
    bool secure_ = false;
    std::chrono::steady_clock::time_point last_use_ = std::chrono::steady_clock::now();
};

/// Connect to a host, automatically selecting TCP or TLS based on secure flag
/// @param host Hostname to connect to
/// @param port Port number
/// @param secure If true, establish TLS connection
/// @param tls_ctx TLS context (required if secure=true)
/// @return Connected stream on success, std::nullopt on error
inline coro::task<std::optional<stream>>
connect(std::string_view host, uint16_t port, bool secure = false,
        tls::tls_context* tls_ctx = nullptr) {
    if (secure) {
        if (!tls_ctx) {
            co_return std::nullopt;
        }
        auto result = co_await tls::tls_connect(*tls_ctx, host, port);
        if (!result) {
            co_return std::nullopt;
        }
        co_return stream(std::move(*result));
    } else {
        auto result = co_await tcp_connect(host, port);
        if (!result) {
            co_return std::nullopt;
        }
        co_return stream(std::move(*result));
    }
}

} // namespace elio::net
