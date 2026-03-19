#pragma once

/// @file client_base.hpp
/// @brief Common configuration and utilities for HTTP-based clients
///
/// This file provides shared infrastructure for HTTP, WebSocket, and SSE clients:
/// - Base client configuration with common settings
/// - TLS context initialization utilities
/// - Connection utility functions

#include <elio/net/stream.hpp>
#include <elio/net/resolve.hpp>
#include <elio/tls/tls_context.hpp>
#include <elio/io/io_context.hpp>
#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>

#include <string>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace elio::http {

namespace detail {

inline size_t next_rotation_offset(const std::string& host, uint16_t port, size_t count) {
    if (count == 0) {
        return 0;
    }

    static std::mutex mutex;
    static std::unordered_map<std::string, size_t> state;

    std::lock_guard<std::mutex> lock(mutex);
    std::string key = host + ":" + std::to_string(port);
    size_t& cursor = state[key];
    size_t offset = cursor % count;
    cursor = (cursor + 1) % count;
    return offset;
}

} // namespace detail

/// Base configuration shared by all HTTP-based clients
/// Can be embedded in more specific configuration structures
struct base_client_config {
    std::chrono::seconds connect_timeout{10};     ///< Connection timeout
    std::chrono::seconds read_timeout{30};        ///< Read timeout
    size_t read_buffer_size = 8192;               ///< Read buffer size
    std::string user_agent;                          ///< User-Agent header (empty = no header)
    bool verify_certificate = true;               ///< Verify TLS certificates
    net::resolve_options resolve_options = net::default_cached_resolve_options();  ///< DNS resolve/cache behavior
    bool rotate_resolved_addresses = true;        ///< Rotate start index across resolved addresses
};

/// Initialize a TLS context for client use with default settings
/// @param ctx TLS context to initialize
/// @param verify_certificate Whether to verify server certificates
inline void init_client_tls_context(tls::tls_context& ctx, bool verify_certificate = true) {
    ctx.use_default_verify_paths();
    if (verify_certificate) {
        ctx.set_verify_mode(tls::verify_mode::peer);
    } else {
        ctx.set_verify_mode(tls::verify_mode::none);
    }
}

/// Connect to a host with TLS context setup
/// @param host Hostname
/// @param port Port number
/// @param secure If true, use TLS
/// @param tls_ctx TLS context (required if secure)
/// @return Connected stream or std::nullopt on error
inline coro::task<std::optional<net::stream>>
client_connect(std::string_view host, uint16_t port, bool secure,
               tls::tls_context* tls_ctx,
               net::resolve_options resolve_opts = net::default_cached_resolve_options(),
               bool rotate_resolved_addresses = true) {

    auto addresses = co_await net::resolve_all(host, port, resolve_opts);
    if (addresses.empty()) {
        ELIO_LOG_ERROR("Failed to resolve {}:{}: {}", host, port, strerror(errno));
        co_return std::nullopt;
    }

    size_t offset = rotate_resolved_addresses
        ? detail::next_rotation_offset(std::string(host), port, addresses.size())
        : 0;

    if (secure) {
        if (!tls_ctx) {
            ELIO_LOG_ERROR("TLS context required for secure connection to {}:{}", host, port);
            co_return std::nullopt;
        }

        for (size_t i = 0; i < addresses.size(); ++i) {
            const auto& addr = addresses[(offset + i) % addresses.size()];
            auto tcp = co_await net::tcp_connect(addr);
            if (!tcp) {
                continue;
            }

            tls::tls_stream tls_stream(std::move(*tcp), *tls_ctx);
            tls_stream.set_hostname(host);
            auto hs = co_await tls_stream.handshake();
            if (!hs) {
                continue;
            }

            co_return net::stream(std::move(tls_stream));
        }

        ELIO_LOG_ERROR("Failed to connect to {}:{}: {}", host, port, strerror(errno));
        co_return std::nullopt;
    } else {
        for (size_t i = 0; i < addresses.size(); ++i) {
            const auto& addr = addresses[(offset + i) % addresses.size()];
            auto result = co_await net::tcp_connect(addr);
            if (result) {
                co_return net::stream(std::move(*result));
            }
        }

        ELIO_LOG_ERROR("Failed to connect to {}:{}: {}", host, port, strerror(errno));
        co_return std::nullopt;
    }
}

} // namespace elio::http
