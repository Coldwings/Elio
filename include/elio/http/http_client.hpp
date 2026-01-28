#pragma once

#include <elio/http/http_common.hpp>
#include <elio/http/http_parser.hpp>
#include <elio/http/http_message.hpp>
#include <elio/http/client_base.hpp>
#include <elio/net/stream.hpp>
#include <elio/io/io_context.hpp>
#include <elio/coro/task.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/log/macros.hpp>

#include <string>
#include <string_view>
#include <memory>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <optional>

namespace elio::http {

/// HTTP client configuration
struct client_config : base_client_config {
    size_t max_redirects = 5;                     ///< Max redirects to follow
    bool follow_redirects = true;                 ///< Auto-follow redirects
    size_t max_connections_per_host = 6;          ///< Max connections per host
    std::chrono::seconds pool_idle_timeout{60};   ///< Idle connection timeout

    client_config() {
        user_agent = "elio-http/1.0";
    }
};

/// Connection wrapper using unified net::stream
using connection = net::stream;

/// Connection pool for HTTP keep-alive
class connection_pool {
public:
    explicit connection_pool(client_config config = {})
        : config_(config) {}
    
    /// Get or create a connection to host
    coro::task<std::optional<connection>> acquire(const std::string& host,
                                                   uint16_t port,
                                                   bool secure,
                                                   tls::tls_context* tls_ctx = nullptr) {
        std::string key = make_key(host, port, secure);

        // Try to get an existing connection
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pools_.find(key);
            if (it != pools_.end() && !it->second.empty()) {
                auto conn = std::move(it->second.front());
                it->second.pop_front();

                // Check if connection is still valid (not too old)
                auto age = std::chrono::steady_clock::now() - conn.last_use();
                if (age < config_.pool_idle_timeout) {
                    conn.touch();
                    co_return std::move(conn);
                }
                // Connection too old, let it close
            }
        }

        // Create new connection using client_connect utility
        auto result = co_await client_connect(host, port, secure, tls_ctx);
        if (!result) {
            co_return std::nullopt;
        }

        co_return std::move(*result);
    }
    
    /// Return a connection to the pool
    void release(const std::string& host, uint16_t port, bool secure, connection conn) {
        std::string key = make_key(host, port, secure);
        
        std::lock_guard<std::mutex> lock(mutex_);
        auto& pool = pools_[key];
        
        if (pool.size() < config_.max_connections_per_host) {
            conn.touch();
            pool.push_back(std::move(conn));
        }
        // Otherwise let connection close
    }
    
    /// Clear all pooled connections
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        pools_.clear();
    }
    
private:
    static std::string make_key(const std::string& host, uint16_t port, bool secure) {
        return (secure ? "https://" : "http://") + host + ":" + std::to_string(port);
    }
    
    client_config config_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::deque<connection>> pools_;
};

/// HTTP client
class client {
public:
    /// Create client with default configuration
    client() : client(client_config{}) {}

    /// Create client with configuration
    explicit client(client_config config)
        : config_(config)
        , pool_(config)
        , tls_ctx_(tls::tls_mode::client) {
        // Setup TLS context using shared utility
        init_client_tls_context(tls_ctx_, config_.verify_certificate);
    }
    
    /// Perform HTTP GET request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> get(std::string_view url_str) {
        return request_url(method::GET, url_str, "", "", coro::cancel_token{});
    }
    
    /// Perform HTTP GET request with cancellation support
    coro::task<std::optional<response>> get(std::string_view url_str, coro::cancel_token token) {
        return request_url(method::GET, url_str, "", "", std::move(token));
    }
    
    /// Perform HTTP POST request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> post(std::string_view url_str, 
                                                   std::string_view body,
                                                   std::string_view content_type = mime::application_form_urlencoded) {
        return request_url(method::POST, url_str, body, content_type, coro::cancel_token{});
    }
    
    /// Perform HTTP POST request with cancellation support
    coro::task<std::optional<response>> post(std::string_view url_str, 
                                                   std::string_view body,
                                                   coro::cancel_token token,
                                                   std::string_view content_type = mime::application_form_urlencoded) {
        return request_url(method::POST, url_str, body, content_type, std::move(token));
    }
    
    /// Perform HTTP PUT request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> put(std::string_view url_str,
                                                  std::string_view body,
                                                  std::string_view content_type = mime::application_json) {
        return request_url(method::PUT, url_str, body, content_type, coro::cancel_token{});
    }
    
    /// Perform HTTP PUT request with cancellation support
    coro::task<std::optional<response>> put(std::string_view url_str,
                                                  std::string_view body,
                                                  coro::cancel_token token,
                                                  std::string_view content_type = mime::application_json) {
        return request_url(method::PUT, url_str, body, content_type, std::move(token));
    }
    
    /// Perform HTTP DELETE request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> del(std::string_view url_str) {
        return request_url(method::DELETE_, url_str, "", "", coro::cancel_token{});
    }
    
    /// Perform HTTP DELETE request with cancellation support
    coro::task<std::optional<response>> del(std::string_view url_str, coro::cancel_token token) {
        return request_url(method::DELETE_, url_str, "", "", std::move(token));
    }
    
    /// Perform HTTP PATCH request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> patch(std::string_view url_str,
                                                    std::string_view body,
                                                    std::string_view content_type = mime::application_json) {
        return request_url(method::PATCH, url_str, body, content_type, coro::cancel_token{});
    }
    
    /// Perform HTTP PATCH request with cancellation support
    coro::task<std::optional<response>> patch(std::string_view url_str,
                                                    std::string_view body,
                                                    coro::cancel_token token,
                                                    std::string_view content_type = mime::application_json) {
        return request_url(method::PATCH, url_str, body, content_type, std::move(token));
    }
    
    /// Perform HTTP HEAD request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> head(std::string_view url_str) {
        return request_url(method::HEAD, url_str, "", "", coro::cancel_token{});
    }
    
    /// Perform HTTP HEAD request with cancellation support
    coro::task<std::optional<response>> head(std::string_view url_str, coro::cancel_token token) {
        return request_url(method::HEAD, url_str, "", "", std::move(token));
    }
    
    /// Send a custom request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> send(request& req, const url& target) {
        return send_request(req, target, 0, coro::cancel_token{});
    }
    
    /// Send a custom request with cancellation support
    coro::task<std::optional<response>> send(request& req, const url& target, coro::cancel_token token) {
        return send_request(req, target, 0, std::move(token));
    }
    
    /// Get TLS context for configuration
    tls::tls_context& tls_context() noexcept { return tls_ctx_; }
    
    /// Get configuration
    client_config& config() noexcept { return config_; }
    const client_config& config() const noexcept { return config_; }
    
private:
    /// Perform request to URL
    coro::task<std::optional<response>> request_url(method m, 
                                                          std::string_view url_str,
                                                          std::string_view body,
                                                          std::string_view content_type,
                                                          coro::cancel_token token) {
        // Check if already cancelled
        if (token.is_cancelled()) {
            errno = ECANCELED;
            co_return std::nullopt;
        }
        
        auto parsed = url::parse(url_str);
        if (!parsed) {
            ELIO_LOG_ERROR("Invalid URL: {}", url_str);
            errno = EINVAL;
            co_return std::nullopt;
        }
        
        request req(m, parsed->path_with_query());
        req.set_host(parsed->authority());
        
        if (!body.empty()) {
            req.set_body(body);
            if (!content_type.empty()) {
                req.set_content_type(content_type);
            }
        }
        
        if (!config_.user_agent.empty()) {
            req.set_header("User-Agent", config_.user_agent);
        }
        
        co_return co_await send_request(req, *parsed, 0, std::move(token));
    }
    
    /// Send request with redirect handling
    coro::task<std::optional<response>> send_request(request& req, const url& target, 
                                                           size_t redirect_count,
                                                           coro::cancel_token token) {
        // Check if cancelled
        if (token.is_cancelled()) {
            errno = ECANCELED;
            co_return std::nullopt;
        }
        
        // Get connection from pool
        auto conn_opt = co_await pool_.acquire(target.host, target.effective_port(),
                                                target.is_secure(), &tls_ctx_);
        if (!conn_opt) {
            errno = ECONNREFUSED;
            co_return std::nullopt;
        }
        
        auto& conn = *conn_opt;
        
        // Ensure Host header is set
        if (req.header("Host").empty()) {
            req.set_host(target.authority());
        }
        
        // Add keep-alive header
        if (!req.get_headers().contains("Connection")) {
            req.set_header("Connection", "keep-alive");
        }
        
        // Serialize and send request
        auto request_data = req.serialize();
        
        ELIO_LOG_DEBUG("Sending request to {}:{}\n{}", target.host, target.effective_port(), request_data);
        
        // Check cancellation before write
        if (token.is_cancelled()) {
            errno = ECANCELED;
            co_return std::nullopt;
        }
        
        auto write_result = co_await conn.write(request_data);
        if (write_result.result <= 0) {
            ELIO_LOG_ERROR("Failed to send request: {}", strerror(-write_result.result));
            errno = -write_result.result;
            co_return std::nullopt;
        }
        
        // Read and parse response
        std::vector<char> buffer(config_.read_buffer_size);
        response_parser parser;
        
        while (!parser.is_complete() && !parser.has_error()) {
            // Check cancellation before read
            if (token.is_cancelled()) {
                errno = ECANCELED;
                co_return std::nullopt;
            }
            
            auto read_result = co_await conn.read(buffer.data(), buffer.size());
            
            if (read_result.result <= 0) {
                if (read_result.result == 0 && parser.is_complete()) {
                    break;  // Clean close after response
                }
                ELIO_LOG_ERROR("Failed to read response: {}", 
                              read_result.result == 0 ? "connection closed" : strerror(-read_result.result));
                errno = read_result.result == 0 ? ECONNRESET : -read_result.result;
                co_return std::nullopt;
            }
            
            auto [result, consumed] = parser.parse(
                std::string_view(buffer.data(), read_result.result));
            
            if (result == parse_result::error) {
                ELIO_LOG_ERROR("Response parse error: {}", parser.error_message());
                errno = EBADMSG;
                co_return std::nullopt;
            }
        }
        
        if (parser.has_error()) {
            ELIO_LOG_ERROR("Response parse error: {}", parser.error_message());
            errno = EBADMSG;
            co_return std::nullopt;
        }
        
        auto resp = response::from_parser(parser);
        
        // Return connection to pool if keep-alive
        if (parser.get_headers().keep_alive(parser.version())) {
            pool_.release(target.host, target.effective_port(), target.is_secure(), std::move(conn));
        }
        
        // Handle redirects
        if (config_.follow_redirects && resp.is_redirect() && redirect_count < config_.max_redirects) {
            auto location = resp.header("Location");
            if (!location.empty()) {
                ELIO_LOG_DEBUG("Following redirect to: {}", location);
                
                // Parse redirect URL
                std::optional<url> redirect_url;
                if (location.starts_with("http://") || location.starts_with("https://")) {
                    redirect_url = url::parse(location);
                } else {
                    // Relative URL
                    url rel;
                    rel.scheme = target.scheme;
                    rel.host = target.host;
                    rel.port = target.port;
                    if (location.starts_with("/")) {
                        rel.path = location;
                    } else {
                        // Resolve relative to current path
                        auto last_slash = target.path.rfind('/');
                        if (last_slash != std::string::npos) {
                            rel.path = target.path.substr(0, last_slash + 1) + std::string(location);
                        } else {
                            rel.path = "/" + std::string(location);
                        }
                    }
                    redirect_url = rel;
                }
                
                if (redirect_url) {
                    // Change method to GET for 303 or POST->GET for 301/302
                    method redirect_method = req.get_method();
                    if (resp.get_status() == status::see_other ||
                        ((resp.get_status() == status::moved_permanently || 
                          resp.get_status() == status::found) && 
                         req.get_method() == method::POST)) {
                        redirect_method = method::GET;
                    }
                    
                    request redirect_req(redirect_method, redirect_url->path_with_query());
                    redirect_req.set_host(redirect_url->authority());
                    if (!config_.user_agent.empty()) {
                        redirect_req.set_header("User-Agent", config_.user_agent);
                    }
                    
                    // Keep body for 307/308
                    if (resp.get_status() == status::temporary_redirect ||
                        resp.get_status() == status::permanent_redirect) {
                        redirect_req.set_body(req.body());
                        auto ct = req.content_type();
                        if (!ct.empty()) {
                            redirect_req.set_content_type(ct);
                        }
                    }
                    
                    co_return co_await send_request(redirect_req, *redirect_url, redirect_count + 1, token);
                }
            }
        }
        
        co_return resp;
    }
    
    client_config config_;
    connection_pool pool_;
    tls::tls_context tls_ctx_;
};

/// Simple convenience functions for one-off requests

/// Perform HTTP GET request
/// @return Response on success, std::nullopt on error (check errno)
inline coro::task<std::optional<response>> get(std::string_view url) {
    client c;
    co_return co_await c.get(url);
}

/// Perform HTTP GET request with cancellation support
inline coro::task<std::optional<response>> get(std::string_view url, coro::cancel_token token) {
    client c;
    co_return co_await c.get(url, std::move(token));
}

/// Perform HTTP POST request
/// @return Response on success, std::nullopt on error (check errno)
inline coro::task<std::optional<response>> post(std::string_view url,
                                                std::string_view body,
                                                std::string_view content_type = mime::application_form_urlencoded) {
    client c;
    co_return co_await c.post(url, body, content_type);
}

/// Perform HTTP POST request with cancellation support
inline coro::task<std::optional<response>> post(std::string_view url,
                                                std::string_view body,
                                                coro::cancel_token token,
                                                std::string_view content_type = mime::application_form_urlencoded) {
    client c;
    co_return co_await c.post(url, body, std::move(token), content_type);
}

} // namespace elio::http
