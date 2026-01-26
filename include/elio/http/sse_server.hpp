#pragma once

/// @file sse_server.hpp
/// @brief Server-Sent Events (SSE) server implementation for Elio
///
/// This file provides SSE server functionality including:
/// - Event streaming to connected clients
/// - Event ID and retry interval support
/// - Named event types
/// - Connection management

#include <elio/http/http_common.hpp>
#include <elio/http/http_message.hpp>
#include <elio/net/tcp.hpp>
#include <elio/tls/tls_stream.hpp>
#include <elio/io/io_context.hpp>
#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>

#include <string>
#include <string_view>
#include <functional>
#include <memory>
#include <variant>
#include <atomic>
#include <sstream>

namespace elio::http::sse {

/// MIME type for SSE
inline constexpr std::string_view SSE_CONTENT_TYPE = "text/event-stream";

/// SSE event structure
struct event {
    std::string id;       ///< Event ID (optional)
    std::string type;     ///< Event type (optional, default is "message")
    std::string data;     ///< Event data (required)
    int retry = -1;       ///< Retry interval in ms (optional, -1 = not set)
    
    /// Create a simple event with just data
    static event message(std::string_view data) {
        return event{"", "", std::string(data), -1};
    }
    
    /// Create a typed event
    static event typed(std::string_view type, std::string_view data) {
        return event{"", std::string(type), std::string(data), -1};
    }
    
    /// Create an event with ID
    static event with_id(std::string_view id, std::string_view data) {
        return event{std::string(id), "", std::string(data), -1};
    }
    
    /// Create a full event
    static event full(std::string_view id, std::string_view type, 
                      std::string_view data, int retry = -1) {
        return event{std::string(id), std::string(type), std::string(data), retry};
    }
};

/// Serialize an SSE event to wire format
inline std::string serialize_event(const event& evt) {
    std::ostringstream ss;
    
    // ID field
    if (!evt.id.empty()) {
        ss << "id: " << evt.id << "\n";
    }
    
    // Event type field
    if (!evt.type.empty()) {
        ss << "event: " << evt.type << "\n";
    }
    
    // Retry field
    if (evt.retry >= 0) {
        ss << "retry: " << evt.retry << "\n";
    }
    
    // Data field (may span multiple lines)
    if (!evt.data.empty()) {
        std::string_view data = evt.data;
        size_t start = 0;
        while (start < data.size()) {
            auto end = data.find('\n', start);
            if (end == std::string_view::npos) {
                ss << "data: " << data.substr(start) << "\n";
                break;
            } else {
                ss << "data: " << data.substr(start, end - start) << "\n";
                start = end + 1;
            }
        }
    } else {
        ss << "data: \n";
    }
    
    // End of event (blank line)
    ss << "\n";
    
    return ss.str();
}

/// SSE connection state
enum class connection_state {
    active,   ///< Connection is active
    closed    ///< Connection is closed
};

/// SSE event stream connection
class sse_connection {
public:
    /// Stream type variant
    using stream_type = std::variant<std::monostate, net::tcp_stream*, tls::tls_stream*>;
    
    /// Create from TCP stream (non-owning pointer)
    explicit sse_connection(net::tcp_stream* tcp)
        : stream_(tcp) {}
    
    /// Create from TLS stream (non-owning pointer)
    explicit sse_connection(tls::tls_stream* tls)
        : stream_(tls) {}
    
    /// Destructor
    ~sse_connection() = default;
    
    // Move only
    sse_connection(sse_connection&&) = default;
    sse_connection& operator=(sse_connection&&) = default;
    sse_connection(const sse_connection&) = delete;
    sse_connection& operator=(const sse_connection&) = delete;
    
    /// Get connection state
    connection_state state() const noexcept { return state_; }
    
    /// Check if connection is active
    bool is_active() const noexcept { return state_ == connection_state::active; }
    
    /// Get last event ID requested by client
    std::string_view last_event_id() const noexcept { return last_event_id_; }
    
    /// Set last event ID (from request header)
    void set_last_event_id(std::string_view id) { last_event_id_ = id; }
    
    /// Send an event
    coro::task<bool> send(const event& evt) {
        if (state_ != connection_state::active) {
            co_return false;
        }
        
        auto data = serialize_event(evt);
        co_return co_await send_raw(data);
    }
    
    /// Send a simple data message
    coro::task<bool> send_data(std::string_view data) {
        co_return co_await send(event::message(data));
    }
    
    /// Send a typed event
    coro::task<bool> send_event(std::string_view type, std::string_view data) {
        co_return co_await send(event::typed(type, data));
    }
    
    /// Send a comment (for keep-alive)
    coro::task<bool> send_comment(std::string_view comment = "") {
        if (state_ != connection_state::active) {
            co_return false;
        }
        
        std::string data = ": ";
        data += comment;
        data += "\n\n";
        
        co_return co_await send_raw(data);
    }
    
    /// Send retry interval
    coro::task<bool> send_retry(int retry_ms) {
        if (state_ != connection_state::active) {
            co_return false;
        }
        
        std::string data = "retry: " + std::to_string(retry_ms) + "\n\n";
        co_return co_await send_raw(data);
    }
    
    /// Close the connection
    void close() {
        state_ = connection_state::closed;
    }
    
    /// Mark connection as active (after sending headers)
    void set_active() {
        state_ = connection_state::active;
    }

private:
    coro::task<bool> send_raw(std::string_view data) {
        size_t sent = 0;
        while (sent < data.size()) {
            io::io_result result;
            
            if (std::holds_alternative<net::tcp_stream*>(stream_)) {
                result = co_await std::get<net::tcp_stream*>(stream_)->write(
                    data.data() + sent, data.size() - sent);
            } else if (std::holds_alternative<tls::tls_stream*>(stream_)) {
                result = co_await std::get<tls::tls_stream*>(stream_)->write(
                    data.data() + sent, data.size() - sent);
            } else {
                co_return false;
            }
            
            if (result.result <= 0) {
                state_ = connection_state::closed;
                co_return false;
            }
            sent += static_cast<size_t>(result.result);
        }
        co_return true;
    }
    
    stream_type stream_;
    connection_state state_ = connection_state::active;
    std::string last_event_id_;
};

/// Build SSE response headers
inline response build_sse_response() {
    response resp(status::ok);
    resp.set_header("Content-Type", SSE_CONTENT_TYPE);
    resp.set_header("Cache-Control", "no-cache");
    resp.set_header("Connection", "keep-alive");
    // Allow CORS for EventSource from any origin
    resp.set_header("Access-Control-Allow-Origin", "*");
    return resp;
}

/// SSE handler function type (receives connection, manages event loop)
using sse_handler_func = std::function<coro::task<void>(sse_connection&)>;

/// SSE endpoint helper for use with HTTP router
/// This is a helper to create SSE handlers compatible with the HTTP server
class sse_endpoint {
public:
    /// Create an SSE endpoint with a handler
    explicit sse_endpoint(sse_handler_func handler)
        : handler_(std::move(handler)) {}
    
    /// Get the handler function
    const sse_handler_func& handler() const { return handler_; }
    
private:
    sse_handler_func handler_;
};

/// Create an SSE response and handler for use with the HTTP server
/// Usage in router:
/// ```cpp
/// router.get("/events", [](context& ctx) -> coro::task<response> {
///     // This endpoint should be handled specially for SSE
///     co_return sse::build_sse_response();
/// });
/// ```
/// 
/// For full SSE support, use the ws_server (WebSocket server) which
/// can be extended to support SSE, or handle SSE at a lower level.

} // namespace elio::http::sse
