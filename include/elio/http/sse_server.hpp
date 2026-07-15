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
#include <elio/coro/cancel_token.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/time/timer.hpp>
#include <elio/log/macros.hpp>

#include <chrono>
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

namespace detail {

inline bool has_sse_line_break(std::string_view value) noexcept {
    return value.find_first_of("\r\n") != std::string_view::npos;
}

template <typename AppendLine>
inline void append_lines_for_sse_field(std::ostringstream& ss,
                                       std::string_view value,
                                       AppendLine append_line) {
    size_t start = 0;
    while (true) {
        size_t end = start;
        while (end < value.size() && value[end] != '\r' && value[end] != '\n') {
            ++end;
        }

        append_line(ss, value.substr(start, end - start));
        if (end == value.size()) {
            break;
        }

        if (value[end] == '\r' && end + 1 < value.size() &&
            value[end + 1] == '\n') {
            start = end + 2;
        } else {
            start = end + 1;
        }

        if (start == value.size()) {
            append_line(ss, std::string_view{});
            break;
        }
    }
}

inline void append_data_lines(std::ostringstream& ss, std::string_view data) {
    append_lines_for_sse_field(
        ss,
        data,
        [](std::ostringstream& out, std::string_view line) {
            out << "data: " << line << "\n";
        });
}

inline std::string serialize_comment(std::string_view comment) {
    std::ostringstream ss;
    if (comment.empty()) {
        ss << ": \n\n";
        return ss.str();
    }

    append_lines_for_sse_field(
        ss,
        comment,
        [](std::ostringstream& out, std::string_view line) {
            out << ": " << line << "\n";
        });
    ss << "\n";
    return ss.str();
}

} // namespace detail

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
    if (!evt.id.empty() && !detail::has_sse_line_break(evt.id)) {
        ss << "id: " << evt.id << "\n";
    }
    
    // Event type field
    if (!evt.type.empty() && !detail::has_sse_line_break(evt.type)) {
        ss << "event: " << evt.type << "\n";
    }
    
    // Retry field
    if (evt.retry >= 0) {
        ss << "retry: " << evt.retry << "\n";
    }
    
    // Data field (may span multiple lines)
    if (!evt.data.empty()) {
        detail::append_data_lines(ss, evt.data);
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
///
/// Concurrency model:
///   - Concurrent producers may call `send`/`send_data`/`send_event`/
///     `send_comment`/`send_retry` from independent coroutines (e.g. an
///     application loop plus a heartbeat task spawned via `run_heartbeat`).
///     A per-connection coroutine mutex serializes the underlying byte
///     writes inside `send_raw` so that the multi-line `event:`/`data:`/
///     `\n\n` framing of one event is never interleaved with another.
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

    // Non-movable: holds a per-connection sync::mutex which is non-movable.
    // The connection is always used by reference inside the SSE handler.
    sse_connection(sse_connection&&) = delete;
    sse_connection& operator=(sse_connection&&) = delete;
    sse_connection(const sse_connection&) = delete;
    sse_connection& operator=(const sse_connection&) = delete;
    
    /// Get connection state
    connection_state state() const noexcept { return state_.load(std::memory_order_acquire); }

    /// Check if connection is active
    bool is_active() const noexcept { return state_.load(std::memory_order_acquire) == connection_state::active; }
    
    /// Get last event ID requested by client
    std::string_view last_event_id() const noexcept { return last_event_id_; }
    
    /// Set last event ID (from request header)
    void set_last_event_id(std::string_view id) { last_event_id_ = id; }
    
    /// Send an event
    coro::task<bool> send(const event& evt) {
        if (state_.load(std::memory_order_acquire) != connection_state::active) {
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
        if (state_.load(std::memory_order_acquire) != connection_state::active) {
            co_return false;
        }
        
        co_return co_await send_raw(detail::serialize_comment(comment));
    }
    
    /// Send retry interval
    coro::task<bool> send_retry(int retry_ms) {
        if (state_.load(std::memory_order_acquire) != connection_state::active) {
            co_return false;
        }
        
        std::string data = "retry: " + std::to_string(retry_ms) + "\n\n";
        co_return co_await send_raw(data);
    }
    
    /// Close the connection
    void close() {
        state_.store(connection_state::closed, std::memory_order_release);
    }

    /// Mark connection as active (after sending headers)
    void set_active() {
        state_.store(connection_state::active, std::memory_order_release);
    }

    /// Heartbeat / keep-alive loop.
    ///
    /// Periodically writes a SSE comment line (`": <comment>\n\n"`).  Many
    /// SSE clients and intermediaries expect a steady stream of bytes to
    /// detect a half-open connection; without a heartbeat a long idle period
    /// looks identical to a stuck server.
    ///
    /// Spawn this alongside the producer loop, e.g.
    /// ```cpp
    /// elio::go([&conn, token]() {
    ///     return conn.run_heartbeat(std::chrono::seconds(30), token);
    /// });
    /// ```
    /// The loop exits when the connection is no longer active, when the
    /// optional cancellation token fires, or when `interval == 0`.
    coro::task<void> run_heartbeat(
        std::chrono::milliseconds interval = std::chrono::seconds(30),
        coro::cancel_token token = {},
        std::string_view comment = "ping") {
        if (interval.count() <= 0) {
            co_return;
        }
        while (state_.load(std::memory_order_acquire) == connection_state::active) {
            auto sleep_result = co_await elio::time::sleep_for(interval, token);
            if (sleep_result == coro::cancel_result::cancelled) {
                co_return;
            }
            if (state_.load(std::memory_order_acquire) != connection_state::active) {
                co_return;
            }
            if (!co_await send_comment(comment)) {
                co_return;
            }
        }
    }

private:
    /// Serialize an entire SSE frame onto the wire.  The per-connection send
    /// mutex guarantees that frames produced by concurrent senders never
    /// interleave at the byte level — without it a heartbeat coroutine and a
    /// data-producer coroutine could splice their `event:`/`data:`/`\n\n`
    /// lines and corrupt the stream.
    coro::task<bool> send_raw(std::string_view data) {
        co_await send_mutex_.lock();
        sync::lock_guard send_guard(send_mutex_);
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
                state_.store(connection_state::closed, std::memory_order_release);
                co_return false;
            }
            sent += static_cast<size_t>(result.result);
        }
        co_return true;
    }

    stream_type stream_;
    std::atomic<connection_state> state_{connection_state::active};
    std::string last_event_id_;
    sync::mutex send_mutex_;  ///< Serializes frame writes; see send_raw().
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
