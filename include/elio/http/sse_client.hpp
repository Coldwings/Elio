#pragma once

/// @file sse_client.hpp
/// @brief Server-Sent Events (SSE) client implementation for Elio
///
/// This file provides SSE client functionality including:
/// - Connection to SSE endpoints (http:// and https://)
/// - Event parsing and delivery
/// - Automatic reconnection support
/// - Last-Event-ID tracking

#include <elio/http/sse_server.hpp>
#include <elio/http/http_common.hpp>
#include <elio/http/http_parser.hpp>
#include <elio/net/tcp.hpp>
#include <elio/tls/tls_context.hpp>
#include <elio/tls/tls_stream.hpp>
#include <elio/io/io_context.hpp>
#include <elio/coro/task.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/time/timer.hpp>
#include <elio/log/macros.hpp>

#include <string>
#include <string_view>
#include <memory>
#include <variant>
#include <optional>
#include <vector>
#include <functional>

namespace elio::http::sse {

/// SSE client configuration
struct client_config {
    std::chrono::seconds connect_timeout{10};     ///< Connection timeout
    size_t read_buffer_size = 4096;               ///< Read buffer size
    int default_retry_ms = 3000;                  ///< Default reconnect interval
    bool auto_reconnect = true;                   ///< Enable auto-reconnection
    size_t max_reconnect_attempts = 0;            ///< Max reconnect attempts (0 = unlimited)
    std::string last_event_id;                    ///< Initial Last-Event-ID
    std::string user_agent = "elio-sse-client/1.0"; ///< User-Agent header
    bool verify_certificate = true;               ///< Verify TLS certificates
};

/// SSE connection state
enum class client_state {
    disconnected,  ///< Not connected
    connecting,    ///< Connection in progress
    connected,     ///< Connected and receiving events
    reconnecting,  ///< Reconnecting after disconnect
    closed         ///< Permanently closed
};

/// SSE event parser
class event_parser {
public:
    event_parser() = default;
    
    /// Parse incoming data and extract events
    /// @param data Input data
    /// @return Number of events parsed
    size_t parse(std::string_view data) {
        buffer_.append(data);
        return process_buffer();
    }
    
    /// Check if an event is available
    bool has_event() const { return !events_.empty(); }
    
    /// Get next event (removes from queue)
    std::optional<event> get_event() {
        if (events_.empty()) return std::nullopt;
        auto evt = std::move(events_.front());
        events_.erase(events_.begin());
        return evt;
    }
    
    /// Get last event ID
    std::string_view last_event_id() const { return last_event_id_; }
    
    /// Get current retry interval
    int retry_ms() const { return retry_ms_; }
    
    /// Reset parser state
    void reset() {
        buffer_.clear();
        current_event_ = event{};
        events_.clear();
        // Don't reset last_event_id_ or retry_ms_ - these persist
    }
    
private:
    size_t process_buffer() {
        size_t events_found = 0;
        
        while (true) {
            // Find line ending
            auto line_end = buffer_.find('\n');
            if (line_end == std::string::npos) {
                break;
            }
            
            std::string line = buffer_.substr(0, line_end);
            buffer_.erase(0, line_end + 1);
            
            // Remove trailing CR if present
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            
            if (line.empty()) {
                // Empty line = dispatch event
                if (!current_event_.data.empty() || !current_event_.type.empty()) {
                    // Remove trailing newline from data
                    if (!current_event_.data.empty() && current_event_.data.back() == '\n') {
                        current_event_.data.pop_back();
                    }
                    
                    // Update last event ID
                    if (!current_event_.id.empty()) {
                        last_event_id_ = current_event_.id;
                    }
                    
                    events_.push_back(std::move(current_event_));
                    current_event_ = event{};
                    ++events_found;
                }
                continue;
            }
            
            // Check for comment
            if (line[0] == ':') {
                // Comment line, ignore
                continue;
            }
            
            // Parse field:value
            auto colon_pos = line.find(':');
            std::string field;
            std::string value;
            
            if (colon_pos == std::string::npos) {
                field = line;
                value = "";
            } else {
                field = line.substr(0, colon_pos);
                value = line.substr(colon_pos + 1);
                // Remove leading space from value
                if (!value.empty() && value[0] == ' ') {
                    value.erase(0, 1);
                }
            }
            
            // Process field
            if (field == "event") {
                current_event_.type = value;
            } else if (field == "data") {
                current_event_.data += value;
                current_event_.data += '\n';
            } else if (field == "id") {
                // ID cannot contain null
                if (value.find('\0') == std::string::npos) {
                    current_event_.id = value;
                }
            } else if (field == "retry") {
                // Parse retry as integer
                int retry = 0;
                bool valid = true;
                for (char c : value) {
                    if (c >= '0' && c <= '9') {
                        retry = retry * 10 + (c - '0');
                    } else {
                        valid = false;
                        break;
                    }
                }
                if (valid && !value.empty()) {
                    retry_ms_ = retry;
                }
            }
            // Ignore unknown fields
        }
        
        return events_found;
    }
    
    std::string buffer_;
    event current_event_;
    std::vector<event> events_;
    std::string last_event_id_;
    int retry_ms_ = 3000;
};

/// SSE client
class sse_client {
public:
    using stream_type = std::variant<std::monostate, net::tcp_stream, tls::tls_stream>;
    
    /// Create SSE client with I/O context
    explicit sse_client(io::io_context& io_ctx, client_config config = {})
        : io_ctx_(&io_ctx)
        , config_(config)
        , tls_ctx_(tls::tls_mode::client) {
        // Setup TLS context
        tls_ctx_.use_default_verify_paths();
        if (config_.verify_certificate) {
            tls_ctx_.set_verify_mode(tls::verify_mode::peer);
        } else {
            tls_ctx_.set_verify_mode(tls::verify_mode::none);
        }
        
        buffer_.resize(config_.read_buffer_size);
        
        if (!config_.last_event_id.empty()) {
            last_event_id_ = config_.last_event_id;
        }
    }
    
    /// Destructor
    ~sse_client() = default;
    
    // Move only
    sse_client(sse_client&&) = default;
    sse_client& operator=(sse_client&&) = default;
    sse_client(const sse_client&) = delete;
    sse_client& operator=(const sse_client&) = delete;
    
    /// Connect to an SSE endpoint
    /// @param url HTTP(S) URL
    /// @return true on success
    coro::task<bool> connect(std::string_view url_str) {
        return connect_impl(url_str, coro::cancel_token{});
    }
    
    /// Connect to an SSE endpoint with cancellation support
    /// @param url HTTP(S) URL
    /// @param token Cancellation token
    /// @return true on success
    coro::task<bool> connect(std::string_view url_str, coro::cancel_token token) {
        return connect_impl(url_str, std::move(token));
    }
    
    /// Get connection state
    client_state state() const noexcept { return state_; }
    
    /// Check if connected
    bool is_connected() const noexcept { return state_ == client_state::connected; }
    
    /// Get last event ID
    std::string_view last_event_id() const noexcept { return last_event_id_; }
    
    /// Receive next event (blocks until event available or connection closed)
    coro::task<std::optional<event>> receive() {
        return receive_impl(coro::cancel_token{});
    }
    
    /// Receive next event with cancellation support
    /// @param token Cancellation token
    /// @return Event on success, std::nullopt on close/error/cancel
    coro::task<std::optional<event>> receive(coro::cancel_token token) {
        return receive_impl(std::move(token));
    }
    
    /// Close the connection
    coro::task<void> close() {
        state_ = client_state::closed;
        
        if (std::holds_alternative<tls::tls_stream>(stream_)) {
            co_await std::get<tls::tls_stream>(stream_).shutdown();
        }
        stream_ = std::monostate{};
    }
    
    /// Get TLS context for configuration
    tls::tls_context& tls_context() noexcept { return tls_ctx_; }
    
    /// Get configuration
    client_config& config() noexcept { return config_; }
    const client_config& config() const noexcept { return config_; }
    
private:
    /// Internal connect implementation
    coro::task<bool> connect_impl(std::string_view url_str, coro::cancel_token token) {
        // Check if already cancelled
        if (token.is_cancelled()) {
            co_return false;
        }
        
        // Parse URL
        auto parsed = url::parse(url_str);
        if (!parsed) {
            ELIO_LOG_ERROR("Invalid SSE URL: {}", url_str);
            co_return false;
        }
        
        url_ = *parsed;
        token_ = std::move(token);
        state_ = client_state::connecting;
        
        co_return co_await do_connect();
    }
    
    /// Internal receive implementation
    coro::task<std::optional<event>> receive_impl(coro::cancel_token token) {
        // Use passed token or stored token
        auto& active_token = token.is_cancelled() ? token_ : token;
        
        while (state_ == client_state::connected) {
            // Check for cancellation
            if (active_token.is_cancelled()) {
                co_return std::nullopt;
            }
            
            // Check for already-parsed events
            if (parser_.has_event()) {
                auto evt = parser_.get_event();
                if (evt && !evt->id.empty()) {
                    last_event_id_ = evt->id;
                }
                co_return evt;
            }
            
            // Read more data
            auto result = co_await read(buffer_.data(), buffer_.size());
            
            if (result.result <= 0) {
                if (result.result == 0) {
                    ELIO_LOG_DEBUG("SSE connection closed by server");
                } else {
                    ELIO_LOG_ERROR("SSE read error: {}", strerror(-result.result));
                }
                
                // Check cancellation before reconnect
                if (active_token.is_cancelled()) {
                    state_ = client_state::disconnected;
                    co_return std::nullopt;
                }
                
                // Handle reconnection
                if (config_.auto_reconnect && state_ != client_state::closed) {
                    state_ = client_state::reconnecting;
                    bool reconnected = co_await try_reconnect();
                    if (reconnected) {
                        continue;
                    }
                }
                
                state_ = client_state::disconnected;
                co_return std::nullopt;
            }
            
            parser_.parse(std::string_view(buffer_.data(), 
                                           static_cast<size_t>(result.result)));
        }
        
        co_return std::nullopt;
    }

    coro::task<bool> do_connect() {
        ELIO_LOG_DEBUG("Connecting to SSE endpoint {}:{}{}", 
                      url_.host, url_.effective_port(), url_.path);
        
        // Establish TCP connection
        if (url_.is_secure()) {
            auto result = co_await tls::tls_connect(tls_ctx_, 
                                                     url_.host, url_.effective_port());
            if (!result) {
                ELIO_LOG_ERROR("Failed to connect to {}:{}: {}", 
                              url_.host, url_.effective_port(), strerror(errno));
                state_ = client_state::disconnected;
                co_return false;
            }
            stream_ = std::move(*result);
        } else {
            auto result = co_await net::tcp_connect(url_.host, 
                                                     url_.effective_port());
            if (!result) {
                ELIO_LOG_ERROR("Failed to connect to {}:{}: {}", 
                              url_.host, url_.effective_port(), strerror(errno));
                state_ = client_state::disconnected;
                co_return false;
            }
            stream_ = std::move(*result);
        }
        
        // Send HTTP request
        std::string request;
        request += "GET ";
        request += url_.path_with_query();
        request += " HTTP/1.1\r\n";
        request += "Host: ";
        request += url_.authority();
        request += "\r\n";
        request += "Accept: text/event-stream\r\n";
        request += "Cache-Control: no-cache\r\n";
        
        if (!config_.user_agent.empty()) {
            request += "User-Agent: ";
            request += config_.user_agent;
            request += "\r\n";
        }
        
        if (!last_event_id_.empty()) {
            request += "Last-Event-ID: ";
            request += last_event_id_;
            request += "\r\n";
        }
        
        request += "\r\n";
        
        auto send_result = co_await write(request.data(), request.size());
        if (send_result.result <= 0) {
            ELIO_LOG_ERROR("Failed to send SSE request");
            state_ = client_state::disconnected;
            co_return false;
        }
        
        // Read response headers
        std::string response_data;
        response_data.reserve(1024);
        
        while (true) {
            auto read_result = co_await read(buffer_.data(), buffer_.size());
            if (read_result.result <= 0) {
                ELIO_LOG_ERROR("Failed to read SSE response");
                state_ = client_state::disconnected;
                co_return false;
            }
            
            response_data.append(buffer_.data(), static_cast<size_t>(read_result.result));
            
            auto header_end = response_data.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                // Parse response
                response_parser parser;
                auto [result, consumed] = parser.parse(response_data);
                
                if (result == parse_result::error) {
                    ELIO_LOG_ERROR("Failed to parse SSE response");
                    state_ = client_state::disconnected;
                    co_return false;
                }
                
                // Check status code
                if (parser.get_status() != status::ok) {
                    ELIO_LOG_ERROR("SSE request failed: {}", 
                                  static_cast<int>(parser.get_status()));
                    state_ = client_state::disconnected;
                    co_return false;
                }
                
                // Check content type
                auto content_type = parser.get_headers().get("Content-Type");
                if (content_type.find("text/event-stream") == std::string_view::npos) {
                    ELIO_LOG_WARNING("Unexpected Content-Type: {}", content_type);
                }
                
                // Feed any remaining data to the event parser
                if (consumed < response_data.size()) {
                    parser_.parse(response_data.substr(consumed));
                }
                
                break;
            }
            
            if (response_data.size() > 8192) {
                ELIO_LOG_ERROR("SSE response headers too large");
                state_ = client_state::disconnected;
                co_return false;
            }
        }
        
        state_ = client_state::connected;
        ELIO_LOG_DEBUG("SSE connected to {}{}", url_.host, url_.path);
        co_return true;
    }
    
    coro::task<bool> try_reconnect() {
        // Reset parser but keep last_event_id
        parser_.reset();
        
        // Close current connection
        if (std::holds_alternative<tls::tls_stream>(stream_)) {
            co_await std::get<tls::tls_stream>(stream_).shutdown();
        }
        stream_ = std::monostate{};
        
        // Get retry interval
        int retry_ms = parser_.retry_ms();
        if (retry_ms <= 0) {
            retry_ms = config_.default_retry_ms;
        }
        
        size_t attempts = 0;
        while (state_ == client_state::reconnecting) {
            // Check for cancellation
            if (token_.is_cancelled()) {
                co_return false;
            }
            
            ++attempts;
            
            if (config_.max_reconnect_attempts > 0 && 
                attempts > config_.max_reconnect_attempts) {
                ELIO_LOG_ERROR("SSE max reconnect attempts exceeded");
                co_return false;
            }
            
            ELIO_LOG_DEBUG("SSE reconnecting (attempt {}) in {}ms...", attempts, retry_ms);
            
            // Wait before reconnecting (cancellable)
            auto result = co_await elio::time::sleep_for(
                std::chrono::milliseconds(retry_ms), token_);
            if (result == coro::cancel_result::cancelled) {
                co_return false;
            }
            
            if (state_ != client_state::reconnecting) {
                co_return false;
            }
            
            // Try to connect
            state_ = client_state::connecting;
            if (co_await do_connect()) {
                co_return true;
            }
            
            state_ = client_state::reconnecting;
            
            // Increase retry interval (exponential backoff, max 1 minute)
            retry_ms = std::min(retry_ms * 2, 60000);
        }
        
        co_return false;
    }
    
    coro::task<io::io_result> read(void* buf, size_t len) {
        if (std::holds_alternative<net::tcp_stream>(stream_)) {
            co_return co_await std::get<net::tcp_stream>(stream_).read(buf, len);
        } else if (std::holds_alternative<tls::tls_stream>(stream_)) {
            co_return co_await std::get<tls::tls_stream>(stream_).read(buf, len);
        }
        co_return io::io_result{-ENOTCONN, 0};
    }
    
    coro::task<io::io_result> write(const void* buf, size_t len) {
        if (std::holds_alternative<net::tcp_stream>(stream_)) {
            co_return co_await std::get<net::tcp_stream>(stream_).write(buf, len);
        } else if (std::holds_alternative<tls::tls_stream>(stream_)) {
            co_return co_await std::get<tls::tls_stream>(stream_).write(buf, len);
        }
        co_return io::io_result{-ENOTCONN, 0};
    }
    
    io::io_context* io_ctx_;
    client_config config_;
    tls::tls_context tls_ctx_;
    stream_type stream_;
    coro::cancel_token token_;  ///< Cancellation token for connection
    
    url url_;
    std::string last_event_id_;
    client_state state_ = client_state::disconnected;
    event_parser parser_;
    std::vector<char> buffer_;
};

/// Convenience function for one-off SSE connection
inline coro::task<std::optional<sse_client>> 
sse_connect(io::io_context& io_ctx, std::string_view url, client_config config = {}) {
    auto client = std::make_optional<sse_client>(io_ctx, config);
    bool success = co_await client->connect(url);
    if (!success) {
        co_return std::nullopt;
    }
    co_return client;
}

/// Convenience function for one-off SSE connection with cancellation support
inline coro::task<std::optional<sse_client>> 
sse_connect(io::io_context& io_ctx, std::string_view url, coro::cancel_token token,
            client_config config = {}) {
    auto client = std::make_optional<sse_client>(io_ctx, config);
    bool success = co_await client->connect(url, std::move(token));
    if (!success) {
        co_return std::nullopt;
    }
    co_return client;
}

} // namespace elio::http::sse
