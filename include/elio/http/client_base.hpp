#pragma once

/// @file client_base.hpp
/// @brief Common configuration and utilities for HTTP-based clients
///
/// This file provides shared infrastructure for HTTP, WebSocket, and SSE clients:
/// - Base client configuration with common settings
/// - TLS context initialization utilities
/// - Connection utility functions

#include <elio/net/stream.hpp>
#include <elio/tls/tls_context.hpp>
#include <elio/io/io_context.hpp>
#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>

#include <string>
#include <chrono>

namespace elio::http {

/// Base configuration shared by all HTTP-based clients
/// Can be embedded in more specific configuration structures
struct base_client_config {
    std::chrono::seconds connect_timeout{10};     ///< Connection timeout
    std::chrono::seconds read_timeout{30};        ///< Read timeout
    size_t read_buffer_size = 8192;               ///< Read buffer size
    std::string user_agent;                          ///< User-Agent header (empty = no header)
    bool verify_certificate = true;               ///< Verify TLS certificates
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
               tls::tls_context* tls_ctx) {
    if (secure) {
        if (!tls_ctx) {
            ELIO_LOG_ERROR("TLS context required for secure connection to {}:{}", host, port);
            co_return std::nullopt;
        }

        auto result = co_await tls::tls_connect(*tls_ctx, host, port);
        if (!result) {
            ELIO_LOG_ERROR("Failed to connect to {}:{}: {}", host, port, strerror(errno));
            co_return std::nullopt;
        }

        co_return net::stream(std::move(*result));
    } else {
        auto result = co_await net::tcp_connect(host, port);
        if (!result) {
            ELIO_LOG_ERROR("Failed to connect to {}:{}: {}", host, port, strerror(errno));
            co_return std::nullopt;
        }

        co_return net::stream(std::move(*result));
    }
}

} // namespace elio::http
