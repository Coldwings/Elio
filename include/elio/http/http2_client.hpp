#pragma once

#include <elio/http/http_common.hpp>
#include <elio/http/http_message.hpp>
#include <elio/http/http2_session.hpp>
#include <elio/net/tcp.hpp>
#include <elio/tls/tls_context.hpp>
#include <elio/tls/tls_stream.hpp>
#include <elio/io/io_context.hpp>
#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>

#include <string>
#include <string_view>
#include <memory>
#include <optional>
#include <unordered_map>
#include <chrono>

namespace elio::http {

/// HTTP/2 client configuration
struct h2_client_config {
    std::chrono::seconds connect_timeout{10};     ///< Connection timeout
    std::chrono::seconds read_timeout{30};        ///< Read timeout
    size_t max_concurrent_streams = 100;          ///< Max concurrent streams per connection
    uint32_t initial_window_size = 65535;         ///< Initial flow control window size
    std::string user_agent = "elio-http2/1.0";    ///< User-Agent header
    bool enable_push = false;                     ///< Enable server push (rarely needed)
};

/// HTTP/2 connection wrapper
class h2_connection {
public:
    h2_connection() = default;
    
    /// Create HTTP/2 connection (takes ownership of TLS stream)
    explicit h2_connection(tls::tls_stream tls)
        : tls_stream_(std::make_unique<tls::tls_stream>(std::move(tls)))
        , session_(std::make_unique<h2_session>(*tls_stream_)) {}
    
    // Move only
    h2_connection(h2_connection&&) = default;
    h2_connection& operator=(h2_connection&&) = default;
    h2_connection(const h2_connection&) = delete;
    h2_connection& operator=(const h2_connection&) = delete;
    
    /// Check if connected
    bool is_connected() const noexcept {
        return tls_stream_ && session_ && session_->is_alive();
    }
    
    /// Get the HTTP/2 session
    h2_session* session() noexcept { return session_.get(); }
    
    /// Get the TLS stream
    tls::tls_stream* tls() noexcept { return tls_stream_.get(); }
    
    /// Close the connection
    coro::task<void> close() {
        if (session_) {
            co_await session_->shutdown();
        }
        if (tls_stream_) {
            co_await tls_stream_->shutdown();
        }
    }
    
    /// Get last use time for connection pooling
    std::chrono::steady_clock::time_point last_use() const noexcept {
        return last_use_;
    }
    
    /// Update last use time
    void touch() {
        last_use_ = std::chrono::steady_clock::now();
    }
    
private:
    std::unique_ptr<tls::tls_stream> tls_stream_;
    std::unique_ptr<h2_session> session_;
    std::chrono::steady_clock::time_point last_use_ = std::chrono::steady_clock::now();
};

/// HTTP/2 client with connection management
class h2_client {
public:
    /// Create HTTP/2 client with I/O context
    explicit h2_client(io::io_context& io_ctx, h2_client_config config = {})
        : io_ctx_(&io_ctx)
        , config_(config)
        , tls_ctx_(tls::tls_mode::client) {
        // Setup TLS context for HTTP/2
        tls_ctx_.use_default_verify_paths();
        tls_ctx_.set_verify_mode(tls::verify_mode::peer);
        // Set ALPN for HTTP/2 (h2 is the HTTP/2 over TLS identifier)
        tls_ctx_.set_alpn_protocols("h2");
    }
    
    /// Perform HTTP/2 GET request
    coro::task<std::optional<response>> get(std::string_view url_str) {
        return request_url(method::GET, url_str, "");
    }
    
    /// Perform HTTP/2 POST request
    coro::task<std::optional<response>> post(std::string_view url_str,
                                             std::string_view body,
                                             std::string_view content_type = mime::application_form_urlencoded) {
        return request_url(method::POST, url_str, body, content_type);
    }
    
    /// Perform HTTP/2 PUT request
    coro::task<std::optional<response>> put(std::string_view url_str,
                                            std::string_view body,
                                            std::string_view content_type = mime::application_json) {
        return request_url(method::PUT, url_str, body, content_type);
    }
    
    /// Perform HTTP/2 DELETE request
    coro::task<std::optional<response>> del(std::string_view url_str) {
        return request_url(method::DELETE_, url_str, "");
    }
    
    /// Perform HTTP/2 PATCH request
    coro::task<std::optional<response>> patch(std::string_view url_str,
                                              std::string_view body,
                                              std::string_view content_type = mime::application_json) {
        return request_url(method::PATCH, url_str, body, content_type);
    }
    
    /// Send a custom request
    coro::task<std::optional<response>> send(method m, const url& target,
                                             std::string_view body = {},
                                             std::string_view content_type = {}) {
        auto conn = co_await get_connection(target.host, target.effective_port());
        if (!conn) {
            co_return std::nullopt;
        }
        
        auto stream_id = conn->session()->submit_request(m, target, body, content_type);
        if (stream_id < 0) {
            ELIO_LOG_ERROR("Failed to submit HTTP/2 request");
            co_return std::nullopt;
        }
        
        auto resp = co_await conn->session()->wait_for_stream(stream_id);
        conn->touch();
        
        // Return connection to pool
        return_connection(target.host, target.effective_port(), std::move(*conn));
        
        co_return resp;
    }
    
    /// Get TLS context for configuration
    tls::tls_context& tls_context() noexcept { return tls_ctx_; }
    
    /// Get configuration
    h2_client_config& config() noexcept { return config_; }
    const h2_client_config& config() const noexcept { return config_; }
    
private:
    /// Perform request to URL
    coro::task<std::optional<response>> request_url(method m,
                                                    std::string_view url_str,
                                                    std::string_view body,
                                                    std::string_view content_type = {}) {
        auto parsed = url::parse(url_str);
        if (!parsed) {
            ELIO_LOG_ERROR("Invalid URL: {}", url_str);
            errno = EINVAL;
            co_return std::nullopt;
        }
        
        // HTTP/2 requires HTTPS (h2)
        // Note: h2c (HTTP/2 cleartext) exists but is rarely used
        if (!parsed->is_secure()) {
            ELIO_LOG_ERROR("HTTP/2 requires HTTPS (h2). Use http:// URLs with HTTP/1.1 client.");
            errno = EPROTONOSUPPORT;
            co_return std::nullopt;
        }
        
        co_return co_await send(m, *parsed, body, content_type);
    }
    
    /// Get or create a connection to host
    coro::task<std::optional<h2_connection>> get_connection(const std::string& host, uint16_t port) {
        std::string key = host + ":" + std::to_string(port);
        
        // Check for existing connection
        auto it = connections_.find(key);
        if (it != connections_.end() && it->second.is_connected()) {
            auto conn = std::move(it->second);
            connections_.erase(it);
            co_return std::move(conn);
        }
        
        // Create new HTTP/2 connection
        // First establish TCP connection
        auto tcp_result = co_await net::tcp_connect(host, port);
        if (!tcp_result) {
            ELIO_LOG_ERROR("Failed to connect to {}:{}", host, port);
            co_return std::nullopt;
        }
        
        // Create TLS stream with ALPN
        tls::tls_stream tls_stream(std::move(*tcp_result), tls_ctx_);
        tls_stream.set_hostname(host);
        
        // Perform TLS handshake
        auto hs_result = co_await tls_stream.handshake();
        if (!hs_result) {
            ELIO_LOG_ERROR("TLS handshake failed for {}:{}", host, port);
            co_return std::nullopt;
        }
        
        // Verify ALPN negotiated h2
        auto alpn = tls_stream.alpn_protocol();
        if (alpn != "h2") {
            ELIO_LOG_ERROR("Server does not support HTTP/2 (ALPN: {})", 
                          alpn.empty() ? "(none)" : std::string(alpn));
            co_return std::nullopt;
        }
        
        ELIO_LOG_DEBUG("HTTP/2 connection established to {}:{}", host, port);
        
        h2_connection conn(std::move(tls_stream));
        
        // Process initial frames (settings exchange)
        if (!co_await conn.session()->process()) {
            ELIO_LOG_ERROR("HTTP/2 session initialization failed");
            co_return std::nullopt;
        }
        
        co_return std::move(conn);
    }
    
    /// Return a connection to the pool
    void return_connection(const std::string& host, uint16_t port, h2_connection conn) {
        if (!conn.is_connected()) {
            return;  // Don't pool dead connections
        }
        
        std::string key = host + ":" + std::to_string(port);
        connections_[key] = std::move(conn);
    }
    
    io::io_context* io_ctx_;
    h2_client_config config_;
    tls::tls_context tls_ctx_;
    std::unordered_map<std::string, h2_connection> connections_;
};

/// Convenience function for one-off HTTP/2 GET request
inline coro::task<std::optional<response>> h2_get(io::io_context& io_ctx, std::string_view url) {
    h2_client client(io_ctx);
    co_return co_await client.get(url);
}

/// Convenience function for one-off HTTP/2 POST request
inline coro::task<std::optional<response>> h2_post(io::io_context& io_ctx,
                                                   std::string_view url,
                                                   std::string_view body,
                                                   std::string_view content_type = mime::application_form_urlencoded) {
    h2_client client(io_ctx);
    co_return co_await client.post(url, body, content_type);
}

} // namespace elio::http
