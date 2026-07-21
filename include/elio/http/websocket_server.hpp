#pragma once

/// @file websocket_server.hpp
/// @brief WebSocket server implementation for Elio
///
/// This file provides WebSocket server functionality including:
/// - HTTP upgrade handling
/// - WebSocket connection management
/// - Message send/receive with async I/O
/// - Ping/pong heartbeat support
/// - Graceful close handling

#include <elio/http/websocket_frame.hpp>
#include <elio/http/websocket_handshake.hpp>
#include <elio/http/http_server.hpp>
#include <elio/http/http_parser.hpp>
#include <elio/net/tcp.hpp>
#include <elio/tls/tls_stream.hpp>
#include <elio/io/io_context.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/log/macros.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <variant>
#include <atomic>
#include <vector>
#include <optional>
#include <unordered_map>

namespace elio::http::websocket {

/// Forward declarations
class ws_connection;

/// WebSocket message type
enum class message_type {
    text,    ///< Text message (UTF-8)
    binary   ///< Binary message
};

/// WebSocket connection state
enum class connection_state {
    connecting,  ///< Handshake in progress
    open,        ///< Connection open
    closing,     ///< Close handshake in progress
    closed       ///< Connection closed
};

/// WebSocket server configuration
struct server_config {
    size_t max_message_size = default_max_message_size; ///< Max message size (16 MiB)
    size_t read_buffer_size = 8192;                  ///< Read buffer size
    std::chrono::seconds ping_interval{30};             ///< 0 = disabled
    std::chrono::seconds ping_timeout{10};              ///< <=0 = no timeout close
    std::vector<std::string> subprotocols;           ///< Supported subprotocols
    bool enable_logging = true;                      ///< Log connections
};

/// WebSocket connection wrapper.
///
/// Concurrency model:
///   - The receive loop (`receive()` / `handle_control_frame`) is **single-reader** —
///     only one coroutine should be awaiting `receive()` on a given connection at a
///     time.  Reads from the underlying stream are not serialized.
///   - Sends (`send_text`, `send_binary`, `send_ping`, `send_pong`, `close`, and the
///     auto-pong / close-response paths inside the receive loop) are serialized by
///     a per-connection coroutine mutex so that frames from concurrent senders never
///     interleave at the byte level.
///   - TLS-backed connections rely on `tls_stream`'s internal SSL-state
///     serialization so the single receive loop may be suspended on socket
///     readiness while a serialized send, including heartbeat pings, writes.
class ws_connection {
public:
    /// Stream type variant
    using stream_type = std::variant<std::monostate, net::tcp_stream*, tls::tls_stream*>;

    /// Create from TCP stream (non-owning pointer)
    explicit ws_connection(net::tcp_stream* tcp)
        : stream_(tcp), is_server_(true) {
        parser_.set_role(endpoint_role::server);
    }

    /// Create from TLS stream (non-owning pointer)
    explicit ws_connection(tls::tls_stream* tls)
        : stream_(tls), is_server_(true) {
        parser_.set_role(endpoint_role::server);
    }

    /// Destructor
    ~ws_connection() = default;

    // Non-movable: holds a per-connection sync::mutex which is non-movable, and
    // the connection is always used by reference inside the request handler.
    ws_connection(ws_connection&&) = delete;
    ws_connection& operator=(ws_connection&&) = delete;
    ws_connection(const ws_connection&) = delete;
    ws_connection& operator=(const ws_connection&) = delete;
    
    /// Get connection state
    connection_state state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }
    
    /// Check if connection is open
    bool is_open() const noexcept { return state() == connection_state::open; }
    
    /// Get negotiated subprotocol
    std::string_view subprotocol() const noexcept { return subprotocol_; }

    /// Get path parameter by name.
    std::string_view param(std::string_view name) const {
        auto it = params_.find(std::string(name));
        if (it != params_.end()) {
            return it->second;
        }
        return {};
    }

    /// Set path parameter.
    void set_param(std::string_view name, std::string_view value) {
        params_[std::string(name)] = std::string(value);
    }

    /// Get all path parameters.
    const std::unordered_map<std::string, std::string>& params() const noexcept {
        return params_;
    }
    
    /// Send a text message
    coro::task<bool> send_text(std::string_view message) {
        if (!is_open()) {
            co_return false;
        }
        auto frame = encode_text_frame(message, !is_server_);
        co_return co_await send_raw(frame);
    }
    
    /// Send a binary message
    coro::task<bool> send_binary(std::string_view data) {
        if (!is_open()) {
            co_return false;
        }
        auto frame = encode_binary_frame(data, !is_server_);
        co_return co_await send_raw(frame);
    }
    
    /// Send a ping
    coro::task<bool> send_ping(std::string_view payload = "") {
        if (!is_open()) {
            co_return false;
        }
        auto frame = encode_ping_frame(payload, !is_server_);
        co_return co_await send_raw(frame);
    }
    
    /// Send a pong
    coro::task<bool> send_pong(std::string_view payload = "") {
        if (!is_open()) {
            co_return false;
        }
        auto frame = encode_pong_frame(payload, !is_server_);
        co_return co_await send_raw(frame);
    }
    
    /// Close the connection
    coro::task<void> close(close_code code = close_code::normal, 
                          std::string_view reason = "") {
        connection_state expected = connection_state::open;
        if (!state_.compare_exchange_strong(expected, connection_state::closing,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            co_return;
        }

        auto frame = encode_close_frame(code, reason, !is_server_);
        co_await send_raw(frame);
        
        // Wait briefly for close acknowledgment
        // In a full implementation, we'd wait for the peer's close frame
        state_.store(connection_state::closed, std::memory_order_release);
    }
    
    /// Receive next message (blocks until message available or connection closed)
    coro::task<std::optional<message>> receive() {
        while (state() == connection_state::open ||
               state() == connection_state::closing) {
            // Process already-parsed frames in wire order.
            while (parser_.next_frame_is_control_frame()) {
                auto control_frame = parser_.get_next_control_frame();
                if (!control_frame) {
                    break;
                }
                co_await handle_control_frame(control_frame->first, control_frame->second);
            }

            if (state() == connection_state::closed) {
                co_return std::nullopt;
            }

            if (parser_.next_frame_is_message()) {
                co_return parser_.get_next_message();
            }

            // Check for errors (RFC 6455 §5.1: emit a close frame before
            // tearing down on protocol violations like an unmasked client
            // frame, oversized message, etc.).
            if (parser_.has_error()) {
                ELIO_LOG_ERROR("WebSocket parse error: {}", parser_.error());
                co_await fail_connection(parser_.error_close_code());
                co_return std::nullopt;
            }

            // Read more data
            auto result = co_await read(buffer_.data(), buffer_.size());

            if (result.result <= 0) {
                if (result.result == 0) {
                    ELIO_LOG_DEBUG("WebSocket connection closed by peer");
                } else {
                    ELIO_LOG_ERROR("WebSocket read error: {}", strerror(-result.result));
                }
                state_.store(connection_state::closed, std::memory_order_release);
                co_return std::nullopt;
            }

            ssize_t parsed = parser_.parse(
                reinterpret_cast<const uint8_t*>(buffer_.data()),
                static_cast<size_t>(result.result)
            );

            if (parsed < 0) {
                ELIO_LOG_ERROR("WebSocket parse error: {}", parser_.error());
                co_await fail_connection(parser_.error_close_code());
                co_return std::nullopt;
            }
        }

        co_return std::nullopt;
    }
    
    /// Set connection as open (after successful handshake)
    void set_open(std::string_view protocol = "") {
        state_.store(connection_state::open, std::memory_order_release);
        subprotocol_ = protocol;
    }
    
    /// Set max message size
    void set_max_message_size(size_t max_size) {
        parser_.set_max_message_size(max_size);
    }

    /// Set read buffer size
    void set_buffer_size(size_t size) {
        buffer_.resize(size);
    }

    /// Hand the parser any bytes that arrived in the same TCP segment as the
    /// HTTP upgrade request (pipelined first frame).  Must be called before
    /// the receive loop starts.  Returns the close code if those bytes already
    /// violate the WebSocket framing contract.
    std::optional<close_code> seed_pipelined_bytes(std::string_view bytes) {
        if (bytes.empty()) return std::nullopt;
        ssize_t parsed = parser_.parse(
            reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        if (parsed < 0 || parser_.has_error()) {
            return parser_.error_close_code();
        }
        return std::nullopt;
    }

    /// Server-side heartbeat loop.
    ///
    /// Sends a ping after each interval and expects receive() to observe at
    /// least one pong before the timeout expires. With timeout enabled, the
    /// window covers both acquiring the serialized send path and writing the
    /// ping frame. The heartbeat never reads from the stream, preserving the
    /// single-reader contract for route handlers.
    coro::task<void> run_heartbeat(
        std::chrono::milliseconds ping_interval,
        std::chrono::milliseconds ping_timeout,
        coro::cancel_token token = {}) {
        if (ping_interval.count() <= 0) {
            co_return;
        }

        while (is_open()) {
            auto interval_result = co_await elio::time::sleep_for(
                ping_interval, token);
            if (interval_result == coro::cancel_result::cancelled || !is_open()) {
                co_return;
            }

            if (ping_timeout.count() <= 0) {
                if (!co_await send_heartbeat_ping(token)) {
                    co_return;
                }
                continue;
            }

            const auto deadline = std::chrono::steady_clock::now() + ping_timeout;
            const auto observed_pongs =
                pong_sequence_.load(std::memory_order_acquire);
            if (!co_await send_heartbeat_ping_until(deadline, token)) {
                if (token.is_cancelled() || !is_open()) {
                    co_return;
                }
                ELIO_LOG_WARNING(
                    "WebSocket heartbeat ping could not be sent within {}ms",
                    ping_timeout.count());
                state_.store(connection_state::closed, std::memory_order_release);
                shutdown_socket();
                co_return;
            }

            auto remaining = remaining_until(deadline);
            if (remaining.count() > 0) {
                auto timeout_result = co_await elio::time::sleep_for(
                    remaining, token);
                if (timeout_result == coro::cancel_result::cancelled ||
                    !is_open()) {
                    co_return;
                }
            }

            if (pong_sequence_.load(std::memory_order_acquire) == observed_pongs) {
                ELIO_LOG_WARNING(
                    "WebSocket heartbeat timed out after {}ms",
                    ping_timeout.count());
                state_.store(connection_state::closed, std::memory_order_release);
                shutdown_socket();
                co_return;
            }
        }
    }

private:
    coro::task<io::io_result> read(void* buf, size_t len) {
        if (std::holds_alternative<net::tcp_stream*>(stream_)) {
            co_return co_await std::get<net::tcp_stream*>(stream_)->read(buf, len);
        } else if (std::holds_alternative<tls::tls_stream*>(stream_)) {
            co_return co_await std::get<tls::tls_stream*>(stream_)->read(buf, len);
        }
        co_return io::io_result{-ENOTCONN, 0};
    }
    
    coro::task<io::io_result> write(const void* buf, size_t len) {
        if (std::holds_alternative<net::tcp_stream*>(stream_)) {
            co_return co_await std::get<net::tcp_stream*>(stream_)->write(buf, len);
        } else if (std::holds_alternative<tls::tls_stream*>(stream_)) {
            co_return co_await std::get<tls::tls_stream*>(stream_)->write(buf, len);
        }
        co_return io::io_result{-ENOTCONN, 0};
    }

    coro::task<io::io_result> write(const void* buf, size_t len,
                                    coro::cancel_token token) {
        if (std::holds_alternative<net::tcp_stream*>(stream_)) {
            co_return co_await std::get<net::tcp_stream*>(stream_)->write(
                buf, len, std::move(token));
        } else if (std::holds_alternative<tls::tls_stream*>(stream_)) {
            co_return co_await std::get<tls::tls_stream*>(stream_)->write(
                buf, len, std::move(token));
        }
        co_return io::io_result{-ENOTCONN, 0};
    }
    
    /// Serialize an entire frame onto the wire.  The per-connection send mutex
    /// guarantees that frames produced by concurrent senders never interleave
    /// at the byte level; the receive loop also funnels its auto-pong /
    /// close-response writes through here, so it competes for the same lock
    /// (and therefore must NOT be called recursively from a write path that
    /// already holds the lock).
    coro::task<bool> send_raw(const std::vector<uint8_t>& data) {
        co_await send_mutex_.lock();
        sync::lock_guard send_guard(send_mutex_);
        size_t sent = 0;
        while (sent < data.size()) {
            auto result = co_await write(data.data() + sent, data.size() - sent);
            if (result.result <= 0) {
                co_return false;
            }
            sent += static_cast<size_t>(result.result);
        }
        co_return true;
    }

    static std::chrono::milliseconds remaining_until(
        std::chrono::steady_clock::time_point deadline) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return std::chrono::milliseconds(0);
        }
        auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now);
        if (remaining.count() <= 0) {
            return std::chrono::milliseconds(1);
        }
        return remaining;
    }

    coro::task<bool> send_heartbeat_ping_until(
        std::chrono::steady_clock::time_point deadline,
        coro::cancel_token token) {
        if (!is_open()) {
            co_return false;
        }
        auto frame = encode_ping_frame("", !is_server_);
        co_return co_await send_raw_until(frame, deadline, std::move(token));
    }

    coro::task<bool> send_heartbeat_ping(coro::cancel_token token) {
        if (!is_open()) {
            co_return false;
        }
        auto frame = encode_ping_frame("", !is_server_);
        co_return co_await send_raw_cancellable(frame, std::move(token));
    }

    coro::task<bool> send_raw_cancellable(
        const std::vector<uint8_t>& data,
        coro::cancel_token token) {
        while (!send_mutex_.try_lock()) {
            if (token.is_cancelled() || !is_open()) {
                co_return false;
            }
            auto result = co_await elio::time::sleep_for(
                std::chrono::milliseconds(10), token);
            if (result == coro::cancel_result::cancelled) {
                co_return false;
            }
        }

        sync::lock_guard send_guard(send_mutex_);
        size_t sent = 0;
        while (sent < data.size()) {
            if (token.is_cancelled() || !is_open()) {
                co_return false;
            }
            auto result = co_await write(
                data.data() + sent, data.size() - sent, token);
            if (result.result <= 0) {
                co_return false;
            }
            sent += static_cast<size_t>(result.result);
        }
        co_return true;
    }

    coro::task<bool> send_raw_until(
        const std::vector<uint8_t>& data,
        std::chrono::steady_clock::time_point deadline,
        coro::cancel_token token) {
        while (!send_mutex_.try_lock()) {
            if (token.is_cancelled() || !is_open() ||
                remaining_until(deadline).count() <= 0) {
                co_return false;
            }

            auto delay = remaining_until(deadline);
            if (delay > std::chrono::milliseconds(10)) {
                delay = std::chrono::milliseconds(10);
            }
            auto result = co_await elio::time::sleep_for(delay, token);
            if (result == coro::cancel_result::cancelled) {
                co_return false;
            }
        }

        sync::lock_guard send_guard(send_mutex_);
        size_t sent = 0;
        while (sent < data.size()) {
            if (token.is_cancelled() || !is_open() ||
                remaining_until(deadline).count() <= 0) {
                co_return false;
            }
            auto result = co_await write_until(
                data.data() + sent, data.size() - sent, deadline, token);
            if (result.result <= 0) {
                co_return false;
            }
            sent += static_cast<size_t>(result.result);
        }
        co_return true;
    }

    coro::task<io::io_result> write_until(
        const void* buf,
        size_t len,
        std::chrono::steady_clock::time_point deadline,
        coro::cancel_token parent_token) {
        auto remaining = remaining_until(deadline);
        if (parent_token.is_cancelled()) {
            co_return io::io_result{-ECANCELED, 0};
        }
        if (remaining.count() <= 0) {
            co_return io::io_result{-ETIMEDOUT, 0};
        }

        auto write_cancel = std::make_shared<coro::cancel_source>();
        auto parent_registration = parent_token.on_cancel([write_cancel]() {
            write_cancel->cancel();
        });

        std::optional<coro::join_handle<void>> timer;
        auto timer_cancel = std::make_shared<coro::cancel_source>();
        if (auto* sched = runtime::scheduler::current()) {
            timer.emplace(sched->go_joinable(
                [write_cancel, timer_cancel, remaining]() -> coro::task<void> {
                    auto result = co_await elio::time::sleep_for(
                        remaining, timer_cancel->get_token());
                    if (result == coro::cancel_result::completed) {
                        write_cancel->cancel();
                    }
                }));
        }

        auto result = co_await write(buf, len, write_cancel->get_token());
        parent_registration.unregister();
        if (timer) {
            timer_cancel->cancel();
            auto handle = std::move(*timer);
            timer.reset();
            co_await std::move(handle);
        }

        co_return result;
    }

    /// Emit a close frame with the given code and tear the connection down.
    /// Used when the parser detects a protocol violation.
    coro::task<void> fail_connection(close_code code) {
        connection_state expected = connection_state::open;
        if (state_.compare_exchange_strong(expected, connection_state::closing,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            auto frame = encode_close_frame(code, "", !is_server_);
            co_await send_raw(frame);
        }
        state_.store(connection_state::closed, std::memory_order_release);
    }

    coro::task<void> handle_control_frame(opcode op, const std::string& payload) {
        switch (op) {
            case opcode::ping:
                co_await send_pong(payload);
                break;
                
            case opcode::pong:
                pong_sequence_.fetch_add(1, std::memory_order_acq_rel);
                ELIO_LOG_DEBUG("Received pong");
                break;
                
            case opcode::close: {
                auto close_info = parse_close_payload(payload);
                auto code = close_info.first;
                [[maybe_unused]] auto& reason = close_info.second;
                ELIO_LOG_DEBUG("WebSocket close received: {} {}",
                              static_cast<uint16_t>(code), reason);

                connection_state expected = connection_state::open;
                if (state_.compare_exchange_strong(
                        expected, connection_state::closing,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    // Send close response.  RFC 6455 §7.4.1 forbids sending
                    // no_status (1005) on the wire; map it to normal (1000).
                    auto resp_code = (code == close_code::no_status)
                                         ? close_code::normal
                                         : code;
                    auto frame = encode_close_frame(resp_code, "", !is_server_);
                    co_await send_raw(frame);
                }
                state_.store(connection_state::closed, std::memory_order_release);
                break;
            }
            
            default:
                break;
        }
    }

    void shutdown_socket() noexcept {
        if (std::holds_alternative<net::tcp_stream*>(stream_)) {
            if (auto* tcp = std::get<net::tcp_stream*>(stream_)) {
                tcp->shutdown_socket();
            }
        } else if (std::holds_alternative<tls::tls_stream*>(stream_)) {
            if (auto* tls = std::get<tls::tls_stream*>(stream_)) {
                tls->shutdown_socket();
            }
        }
    }
    
    stream_type stream_;
    std::atomic<connection_state> state_{connection_state::connecting};
    bool is_server_ = true;
    std::string subprotocol_;
    std::unordered_map<std::string, std::string> params_;
    frame_parser parser_;
    std::vector<char> buffer_ = std::vector<char>(8192);
    sync::mutex send_mutex_;  ///< Serializes frame writes; see class comment.
    std::atomic<uint64_t> pong_sequence_{0};
};

/// WebSocket upgrade handler result
struct upgrade_result {
    bool success = false;
    std::string error;
    std::string accepted_protocol;
};

/// Perform WebSocket upgrade on an HTTP request
/// @param req The incoming HTTP request
/// @param config Server configuration
/// @return Upgrade result with handshake response info
inline upgrade_result validate_upgrade_request(const request& req,
                                               const server_config& config) {
    upgrade_result result;
    
    // Check for WebSocket upgrade request
    auto upgrade = req.header("Upgrade");
    auto connection = req.header("Connection");
    auto ws_key = req.header("Sec-WebSocket-Key");
    auto ws_version = req.header("Sec-WebSocket-Version");
    
    if (!is_websocket_upgrade(method_to_string(req.get_method()),
                              upgrade, connection, ws_version)) {
        result.error = "Not a valid WebSocket upgrade request";
        return result;
    }
    
    // Validate Sec-WebSocket-Key
    if (ws_key.empty()) {
        result.error = "Missing Sec-WebSocket-Key header";
        return result;
    }
    
    if (!is_valid_websocket_key(ws_key)) {
        result.error = "Invalid Sec-WebSocket-Key";
        return result;
    }
    
    // Negotiate subprotocol
    auto client_protocols_header = req.header("Sec-WebSocket-Protocol");
    if (!client_protocols_header.empty()) {
        auto client_protocols = parse_protocols(client_protocols_header);
        result.accepted_protocol = negotiate_protocol(client_protocols, config.subprotocols);
    }
    
    result.success = true;
    return result;
}

/// Build WebSocket upgrade response
/// @param key The client's Sec-WebSocket-Key
/// @param protocol The negotiated subprotocol (empty if none)
/// @return HTTP 101 response
inline response build_upgrade_response(std::string_view key, 
                                       std::string_view protocol = "") {
    auto accept = compute_websocket_accept(key);
    
    response resp(status::switching_protocols);
    resp.set_header("Upgrade", "websocket");
    resp.set_header("Connection", "Upgrade");
    resp.set_header("Sec-WebSocket-Accept", accept);
    
    if (!protocol.empty()) {
        resp.set_header("Sec-WebSocket-Protocol", protocol);
    }
    
    return resp;
}

/// WebSocket handler function type
using ws_handler_func = std::function<coro::task<void>(ws_connection&)>;

/// WebSocket route for the HTTP router.  Uses the same compiled segment
/// representation as http::route to avoid the ReDoS surface and per-request
/// std::regex_match cost that the HTTP router shed in commit bc511be.
struct ws_route {
    std::string pattern;
    std::vector<route_segment> segments;
    std::vector<std::string> param_names;  ///< Captured parameter names.
    ws_handler_func handler;
    server_config config;

    /// Match `path` against the compiled segments, populating `params` with
    /// any captured `:name` components.  Mirrors http::route::match exactly.
    bool match(std::string_view path,
               std::unordered_map<std::string, std::string>& params) const {
        std::vector<std::string_view> components;
        components.reserve(8);
        size_t start = 0;
        for (size_t i = 0; i <= path.size(); ++i) {
            if (i == path.size() || path[i] == '/') {
                components.emplace_back(path.data() + start, i - start);
                start = i + 1;
            }
        }

        size_t si = 0;
        size_t ci = 0;
        while (si < segments.size()) {
            const auto& seg = segments[si];
            if (seg.kind == segment_kind::wildcard) {
                return ci < components.size();
            }
            if (ci >= components.size()) return false;
            if (seg.kind == segment_kind::literal) {
                if (components[ci] != seg.value) return false;
            } else {
                if (components[ci].empty()) return false;
                params[seg.value] = std::string(components[ci]);
            }
            ++si;
            ++ci;
        }
        return ci == components.size();
    }
};

/// Extended HTTP router with WebSocket support
class ws_router : public router {
public:
    ws_router() = default;

    /// Add a WebSocket route.  Pattern grammar matches the HTTP router:
    ///   - `:name` captures one path component; handlers read it from
    ///     `ws_connection::param("name")`
    ///   - trailing `*` matches the rest of the path
    ///   - anything else is a literal component
    void websocket(std::string_view pattern, ws_handler_func handler,
                   server_config config = {}) {
        ws_route route;
        route.pattern = pattern;
        route.handler = std::move(handler);
        route.config = config;

        // Compile the pattern into a flat segment list, splitting on '/'
        // identically to ws_route::match() so empty leading/trailing
        // components round-trip correctly.
        size_t start = 0;
        for (size_t i = 0; i <= pattern.size(); ++i) {
            if (i == pattern.size() || pattern[i] == '/') {
                std::string_view comp(pattern.data() + start, i - start);
                route_segment seg;
                if (!comp.empty() && comp.front() == ':') {
                    seg.kind = segment_kind::param;
                    seg.value.assign(comp.data() + 1, comp.size() - 1);
                    route.param_names.push_back(seg.value);
                } else if (comp == "*") {
                    if (i != pattern.size()) {
                        throw std::invalid_argument(
                            "WebSocket route wildcard must be the final path segment");
                    }
                    seg.kind = segment_kind::wildcard;
                } else {
                    seg.kind = segment_kind::literal;
                    seg.value.assign(comp.data(), comp.size());
                }
                route.segments.push_back(std::move(seg));
                start = i + 1;
            }
        }

        ws_routes_.push_back(std::move(route));
    }

    /// Find matching WebSocket route
    const ws_route* find_ws_route(
        std::string_view path,
        std::unordered_map<std::string, std::string>& params) const {
        for (const auto& route : ws_routes_) {
            params.clear();
            if (route.match(path, params)) {
                return &route;
            }
        }
        return nullptr;
    }

    const ws_route* find_ws_route(std::string_view path) const {
        std::unordered_map<std::string, std::string> params;
        return find_ws_route(path, params);
    }

    /// Check if path matches a WebSocket route
    bool is_websocket_path(std::string_view path) const {
        return find_ws_route(path) != nullptr;
    }

private:
    std::vector<ws_route> ws_routes_;
};

/// WebSocket-enabled HTTP server
class ws_server {
public:
    /// Create server with WebSocket-enabled router
    explicit ws_server(ws_router r, http::server_config http_config = {})
        : router_(std::move(r)), http_config_(http_config) {}
    
    /// Start listening on address (plain HTTP/WS)
    coro::task<void> listen(const net::socket_address& addr,
                            const net::tcp_options& opts = {}) {
        const auto start_epoch = stop_epoch_.load(std::memory_order_acquire);
        return listen_impl(addr, opts, start_epoch);
    }

    /// Start listening with TLS (HTTPS/WSS)
    /// @note The caller must ensure `tls_ctx` outlives all spawned connection
    ///       handlers.  See http::server::listen_tls for details.
    coro::task<void> listen_tls(const net::socket_address& addr, tls::tls_context& tls_ctx,
                                const net::tcp_options& opts = {}) {
        const auto start_epoch = stop_epoch_.load(std::memory_order_acquire);
        return listen_tls_impl(addr, tls_ctx, opts, start_epoch);
    }

    /// Stop the server
    void stop() {
        cancel_active_accepts();
    }

    /// Check if server is running
    bool is_running() const noexcept { return running_; }

    /// Return the number of in-flight connection handlers.  Callers that
    /// destroy the server after stop() should wait until this returns 0
    /// to avoid use-after-free on router_, http_config_, etc.
    size_t active_connections() const noexcept {
        return active_connections_.load(std::memory_order_acquire);
    }

private:
    coro::task<void> listen_impl(const net::socket_address& addr,
                                 const net::tcp_options& opts,
                                 size_t start_epoch) {
        auto* sched = runtime::scheduler::current();
        if (!sched) {
            ELIO_LOG_ERROR("WebSocket server must be started from within a scheduler context");
            co_return;
        }

        auto listener_result = net::tcp_listener::bind(addr, opts);
        if (!listener_result) {
            ELIO_LOG_ERROR("Failed to bind WebSocket server: {}", strerror(errno));
            co_return;
        }

        ELIO_LOG_INFO("WebSocket server listening on {}", addr.to_string());

        auto& listener = *listener_result;
        auto accept_source = begin_accept_loop(start_epoch);
        if (!accept_source) {
            co_return;
        }
        auto accept_token = accept_source->get_token();

        while (running_.load(std::memory_order_acquire)) {
            auto stream_result = co_await listener.accept(accept_token);
            if (accept_loop_should_stop(accept_token)) {
                break;
            }
            if (!stream_result) {
                if (running_.load(std::memory_order_acquire)) {
                    ELIO_LOG_ERROR("Accept error: {}", strerror(errno));
                }
                continue;
            }

            // Spawn connection handler (tracked for graceful shutdown)
            active_connections_.fetch_add(1, std::memory_order_relaxed);
            sched->go([this, s = std::move(*stream_result)]() mutable {
                return handle_connection_guarded(std::move(s));
            });
        }
    }

    coro::task<void> listen_tls_impl(const net::socket_address& addr,
                                     tls::tls_context& tls_ctx,
                                     const net::tcp_options& opts,
                                     size_t start_epoch) {
        auto* sched = runtime::scheduler::current();
        if (!sched) {
            ELIO_LOG_ERROR("Secure WebSocket server must be started from within a scheduler context");
            co_return;
        }

        auto listener_result = net::tcp_listener::bind(addr, opts);
        if (!listener_result) {
            ELIO_LOG_ERROR("Failed to bind secure WebSocket server: {}", strerror(errno));
            co_return;
        }

        ELIO_LOG_INFO("Secure WebSocket server listening on {}", addr.to_string());

        auto& listener = *listener_result;
        auto accept_source = begin_accept_loop(start_epoch);
        if (!accept_source) {
            co_return;
        }
        auto accept_token = accept_source->get_token();

        // Capture by pointer — see doc comment above for lifetime requirement.
        auto* tls_ctx_ptr = &tls_ctx;

        while (running_.load(std::memory_order_acquire)) {
            auto stream_result = co_await listener.accept(accept_token);
            if (accept_loop_should_stop(accept_token)) {
                break;
            }
            if (!stream_result) {
                if (running_.load(std::memory_order_acquire)) {
                    ELIO_LOG_ERROR("Accept error: {}", strerror(errno));
                }
                continue;
            }

            // Spawn TLS connection handler (tracked for graceful shutdown)
            active_connections_.fetch_add(1, std::memory_order_relaxed);
            sched->go([this, s = std::move(*stream_result), tls_ctx_ptr]() mutable {
                return handle_tls_connection_guarded(std::move(s), *tls_ctx_ptr);
            });
        }
    }

    class active_connection_guard {
    public:
        explicit active_connection_guard(std::atomic<size_t>& active) noexcept
            : active_(active) {}

        ~active_connection_guard() {
            active_.fetch_sub(1, std::memory_order_relaxed);
        }

        active_connection_guard(const active_connection_guard&) = delete;
        active_connection_guard& operator=(const active_connection_guard&) = delete;

    private:
        std::atomic<size_t>& active_;
    };

    /// Guard wrapper that decrements the active-connection counter on exit.
    coro::task<void> handle_connection_guarded(net::tcp_stream stream) {
        active_connection_guard guard(active_connections_);
        co_await handle_connection(std::move(stream));
    }

    /// Guard wrapper for TLS connections.
    coro::task<void> handle_tls_connection_guarded(net::tcp_stream tcp,
                                                   tls::tls_context& tls_ctx) {
        active_connection_guard guard(active_connections_);
        co_await handle_tls_connection(std::move(tcp), tls_ctx);
    }

    /// Handle a plain connection
    coro::task<void> handle_connection(net::tcp_stream stream) {
        auto peer = stream.peer_address();
        std::string client_addr = peer ? peer->to_string() : "unknown";
        
        if (http_config_.enable_logging) {
            ELIO_LOG_DEBUG("Connection from {}", client_addr);
        }
        
        co_await handle_request(stream, client_addr);
    }
    
    /// Handle a TLS connection
    coro::task<void> handle_tls_connection(net::tcp_stream tcp, tls::tls_context& tls_ctx) {
        auto peer = tcp.peer_address();
        std::string client_addr = peer ? peer->to_string() : "unknown";
        
        if (http_config_.enable_logging) {
            ELIO_LOG_DEBUG("Secure connection from {}", client_addr);
        }
        
        tls::tls_stream stream(std::move(tcp), tls_ctx);
        auto hs_result = co_await http::detail::perform_tls_handshake_with_timeout(
            stream, http_config_.keep_alive_timeout);
        if (!hs_result.ok) {
            if (hs_result.timed_out) {
                ELIO_LOG_ERROR("TLS handshake timed out for {}", client_addr);
            } else {
                ELIO_LOG_ERROR("TLS handshake failed for {}", client_addr);
            }
            co_return;
        }
        
        co_await handle_request(stream, client_addr);
        
        co_await stream.shutdown();
    }
    
    /// Handle HTTP request (check for WebSocket upgrade)
    template<typename Stream>
    coro::task<void> handle_request(Stream& stream, const std::string& client_addr) {
        auto* sched = runtime::scheduler::current();
        std::vector<char> buffer(http_config_.read_buffer_size);
        request_parser parser;
        parser.set_max_headers(http_config_.max_headers);
        parser.set_max_header_size(http_config_.max_header_size);

        auto timed_out = std::make_shared<std::atomic<bool>>(false);
        auto cancel_src = std::make_shared<coro::cancel_source>();
        auto* stream_ptr = &stream;
        auto timeout = http_config_.keep_alive_timeout;
        std::optional<coro::join_handle<void>> watchdog;
        if (sched && timeout.count() > 0) {
            watchdog.emplace(sched->go_joinable(
                [stream_ptr, timed_out, cancel_src, timeout]() -> coro::task<void> {
                    auto r = co_await elio::time::sleep_for(
                        timeout, cancel_src->get_token());
                    if (r == coro::cancel_result::completed) {
                        timed_out->store(true, std::memory_order_release);
                        stream_ptr->shutdown_socket();
                    }
                    co_return;
                }));
        }

        auto stop_watchdog = [&]() -> coro::task<void> {
            if (watchdog) {
                cancel_src->cancel();
                auto wd = std::move(*watchdog);
                watchdog.reset();
                co_await std::move(wd);
            }
            co_return;
        };

        // Read HTTP request
        size_t current_request_size = 0;
        while (!parser.is_complete() && !parser.has_error()) {
            auto result = co_await stream.read(buffer.data(), buffer.size());

            if (timed_out->load(std::memory_order_acquire)) {
                co_await stop_watchdog();
                co_return;
            }
            if (result.result <= 0) {
                co_await stop_watchdog();
                co_return;
            }

            auto [parse_result, consumed] = parser.parse(
                std::string_view(buffer.data(),
                                 static_cast<size_t>(result.result)));
            current_request_size += consumed;

            size_t effective_request_size = current_request_size;
            if (!parser.is_complete()) {
                effective_request_size += parser.buffered_input_size();
            }

            if (effective_request_size > http_config_.max_request_size ||
                parser.body().size() > http_config_.max_request_size) {
                ELIO_LOG_WARNING("WebSocket upgrade request exceeds max_request_size");
                co_await stop_watchdog();
                auto resp = response(status::payload_too_large, "Payload Too Large");
                resp.set_header("Connection", "close");
                co_await send_response(stream, resp, parser.get_method());
                co_return;
            }

            if (parse_result == parse_result::error) {
                co_await stop_watchdog();
                co_return;
            }
        }

        co_await stop_watchdog();
        
        if (timed_out->load(std::memory_order_acquire) || parser.has_error()) {
            co_return;
        }
        
        auto req = request::from_parser(parser);
        
        // Check for WebSocket upgrade
        std::unordered_map<std::string, std::string> ws_params;
        auto* ws_route = router_.find_ws_route(req.path(), ws_params);
        
        if (ws_route) {
            // Handle WebSocket upgrade
            auto upgrade = validate_upgrade_request(req, ws_route->config);
            
            if (!upgrade.success) {
                ELIO_LOG_WARNING("WebSocket upgrade failed: {}", upgrade.error);
                auto resp = response::bad_request(upgrade.error);
                co_await send_response(stream, resp, req.get_method());
                co_return;
            }
            
            // Capture any bytes that arrived in the same TCP segment as the
            // upgrade request before we let the request_parser go out of
            // scope — they belong to the first WebSocket frame and must not
            // be discarded.
            std::string pipelined = parser.take_remaining();

            // Send upgrade response
            auto ws_key = req.header("Sec-WebSocket-Key");
            auto resp = build_upgrade_response(ws_key, upgrade.accepted_protocol);
            co_await send_response(stream, resp, req.get_method());

            if (http_config_.enable_logging) {
                ELIO_LOG_INFO("WebSocket upgrade: {} from {}", req.path(), client_addr);
            }

            // Create WebSocket connection and run handler
            ws_connection conn(&stream);
            conn.set_open(upgrade.accepted_protocol);
            conn.set_max_message_size(ws_route->config.max_message_size);
            conn.set_buffer_size(ws_route->config.read_buffer_size);
            if (auto close_code = conn.seed_pipelined_bytes(pipelined)) {
                ELIO_LOG_ERROR("WebSocket pipelined first frame is invalid");
                co_await conn.close(*close_code);
                co_return;
            }
            for (const auto& [name, value] : ws_params) {
                conn.set_param(name, value);
            }

            std::optional<coro::join_handle<void>> heartbeat;
            auto heartbeat_cancel = std::make_shared<coro::cancel_source>();
            if (sched && ws_route->config.ping_interval.count() > 0) {
                auto ping_interval =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        ws_route->config.ping_interval);
                auto ping_timeout =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        ws_route->config.ping_timeout);
                heartbeat.emplace(sched->go_joinable(
                    [&conn,
                     heartbeat_cancel,
                     ping_interval,
                     ping_timeout]() -> coro::task<void> {
                        co_await conn.run_heartbeat(
                            ping_interval,
                            ping_timeout,
                            heartbeat_cancel->get_token());
                    }));
            }

            auto stop_heartbeat = [&]() -> coro::task<void> {
                if (heartbeat) {
                    heartbeat_cancel->cancel();
                    auto hb = std::move(*heartbeat);
                    heartbeat.reset();
                    co_await std::move(hb);
                }
                co_return;
            };
            
            try {
                co_await ws_route->handler(conn);
            } catch (const std::exception& e) {
                ELIO_LOG_ERROR("WebSocket handler exception: {}", e.what());
            } catch (...) {
                ELIO_LOG_ERROR("WebSocket handler unknown exception");
            }

            co_await stop_heartbeat();
            
            // Ensure connection is closed
            if (conn.is_open()) {
                co_await conn.close(close_code::going_away);
            }
        } else {
            // Handle as regular HTTP request
            context ctx(std::move(req), client_addr);
            
            std::unordered_map<std::string, std::string> params;
            auto* http_route = router_.find_route(ctx.req().get_method(), 
                                                   ctx.req().path(), params);
            
            response resp;
            if (http_route) {
                for (const auto& [name, value] : params) {
                    ctx.set_param(name, value);
                }
                try {
                    resp = co_await http_route->handler(ctx);
                } catch (const std::exception& e) {
                    ELIO_LOG_ERROR("HTTP handler exception: {}", e.what());
                    resp = response::internal_error();
                }
            } else {
                resp = response::not_found();
            }
            
            co_await send_response(stream, resp, ctx.req().get_method());
        }
    }
    
    /// Send HTTP response
    template<typename Stream>
    coro::task<void> send_response(Stream& stream,
                                   const response& resp,
                                   std::optional<method> request_method = std::nullopt) {
        auto data = request_method ? resp.serialize(*request_method)
                                   : resp.serialize();
        
        size_t sent = 0;
        while (sent < data.size()) {
            auto result = co_await stream.write(data.data() + sent, data.size() - sent);
            if (result.result <= 0) {
                break;
            }
            sent += result.result;
        }
    }

    std::shared_ptr<coro::cancel_source> begin_accept_loop(size_t start_epoch) {
        auto source = std::make_shared<coro::cancel_source>();
        std::lock_guard<std::mutex> lock(accept_cancel_mutex_);
        if (stop_epoch_.load(std::memory_order_acquire) != start_epoch) {
            return {};
        }
        auto it = active_accept_sources_.begin();
        while (it != active_accept_sources_.end()) {
            if (it->expired()) {
                it = active_accept_sources_.erase(it);
            } else {
                ++it;
            }
        }
        active_accept_sources_.push_back(source);
        running_.store(true, std::memory_order_release);
        return source;
    }

    void cancel_active_accepts() {
        std::vector<std::shared_ptr<coro::cancel_source>> sources;
        {
            std::lock_guard<std::mutex> lock(accept_cancel_mutex_);
            stop_epoch_.fetch_add(1, std::memory_order_acq_rel);
            running_.store(false, std::memory_order_release);
            auto it = active_accept_sources_.begin();
            while (it != active_accept_sources_.end()) {
                if (auto source = it->lock()) {
                    sources.push_back(std::move(source));
                    ++it;
                } else {
                    it = active_accept_sources_.erase(it);
                }
            }
        }

        for (auto& source : sources) {
            source->cancel();
        }
    }

    bool accept_loop_should_stop(const coro::cancel_token& accept_token) noexcept {
        if (!running_.load(std::memory_order_acquire) ||
            accept_token.is_cancelled()) {
            running_.store(false, std::memory_order_release);
            return true;
        }
        return false;
    }
    
    ws_router router_;
    http::server_config http_config_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> active_connections_{0};  ///< In-flight connection handlers
    std::atomic<size_t> stop_epoch_{0};
    mutable std::mutex accept_cancel_mutex_;
    std::vector<std::weak_ptr<coro::cancel_source>> active_accept_sources_;
};

} // namespace elio::http::websocket
