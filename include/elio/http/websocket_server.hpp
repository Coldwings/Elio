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
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/log/macros.hpp>

#include <string>
#include <string_view>
#include <functional>
#include <memory>
#include <variant>
#include <atomic>
#include <vector>
#include <optional>

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
    size_t max_message_size = 16 * 1024 * 1024;  ///< Max message size (16MB)
    size_t read_buffer_size = 8192;               ///< Read buffer size
    std::chrono::seconds ping_interval{30};       ///< Ping interval (0 = disabled)
    std::chrono::seconds ping_timeout{10};        ///< Pong timeout
    std::vector<std::string> subprotocols;        ///< Supported subprotocols
    bool enable_logging = true;                   ///< Log connections
};

/// WebSocket connection wrapper
class ws_connection {
public:
    /// Stream type variant
    using stream_type = std::variant<std::monostate, net::tcp_stream*, tls::tls_stream*>;
    
    /// Create from TCP stream (non-owning pointer)
    explicit ws_connection(net::tcp_stream* tcp)
        : stream_(tcp), is_server_(true) {}
    
    /// Create from TLS stream (non-owning pointer)
    explicit ws_connection(tls::tls_stream* tls)
        : stream_(tls), is_server_(true) {}
    
    /// Destructor
    ~ws_connection() = default;
    
    // Move only
    ws_connection(ws_connection&&) = default;
    ws_connection& operator=(ws_connection&&) = default;
    ws_connection(const ws_connection&) = delete;
    ws_connection& operator=(const ws_connection&) = delete;
    
    /// Get connection state
    connection_state state() const noexcept { return state_; }
    
    /// Check if connection is open
    bool is_open() const noexcept { return state_ == connection_state::open; }
    
    /// Get negotiated subprotocol
    std::string_view subprotocol() const noexcept { return subprotocol_; }
    
    /// Send a text message
    coro::task<bool> send_text(std::string_view message) {
        if (state_ != connection_state::open) {
            co_return false;
        }
        auto frame = encode_text_frame(message, !is_server_);
        co_return co_await send_raw(frame);
    }
    
    /// Send a binary message
    coro::task<bool> send_binary(std::string_view data) {
        if (state_ != connection_state::open) {
            co_return false;
        }
        auto frame = encode_binary_frame(data, !is_server_);
        co_return co_await send_raw(frame);
    }
    
    /// Send a ping
    coro::task<bool> send_ping(std::string_view payload = "") {
        if (state_ != connection_state::open) {
            co_return false;
        }
        auto frame = encode_ping_frame(payload, !is_server_);
        co_return co_await send_raw(frame);
    }
    
    /// Send a pong
    coro::task<bool> send_pong(std::string_view payload = "") {
        if (state_ != connection_state::open) {
            co_return false;
        }
        auto frame = encode_pong_frame(payload, !is_server_);
        co_return co_await send_raw(frame);
    }
    
    /// Close the connection
    coro::task<void> close(close_code code = close_code::normal, 
                          std::string_view reason = "") {
        if (state_ != connection_state::open) {
            co_return;
        }
        
        state_ = connection_state::closing;
        
        auto frame = encode_close_frame(code, reason, !is_server_);
        co_await send_raw(frame);
        
        // Wait briefly for close acknowledgment
        // In a full implementation, we'd wait for the peer's close frame
        state_ = connection_state::closed;
    }
    
    /// Receive next message (blocks until message available or connection closed)
    coro::task<std::optional<message>> receive() {
        while (state_ == connection_state::open || state_ == connection_state::closing) {
            // Check for already-parsed messages
            if (parser_.has_message()) {
                co_return parser_.get_message();
            }
            
            // Process control frames
            while (parser_.has_control_frame()) {
                auto [op, payload] = *parser_.get_control_frame();
                co_await handle_control_frame(op, payload);
            }
            
            // Check for errors
            if (parser_.has_error()) {
                ELIO_LOG_ERROR("WebSocket parse error: {}", parser_.error());
                state_ = connection_state::closed;
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
                state_ = connection_state::closed;
                co_return std::nullopt;
            }
            
            int parsed = parser_.parse(
                reinterpret_cast<const uint8_t*>(buffer_.data()),
                static_cast<size_t>(result.result)
            );
            
            if (parsed < 0) {
                ELIO_LOG_ERROR("WebSocket parse error: {}", parser_.error());
                state_ = connection_state::closed;
                co_return std::nullopt;
            }
        }
        
        co_return std::nullopt;
    }
    
    /// Set connection as open (after successful handshake)
    void set_open(std::string_view protocol = "") {
        state_ = connection_state::open;
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
    
    coro::task<bool> send_raw(const std::vector<uint8_t>& data) {
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
    
    coro::task<void> handle_control_frame(opcode op, const std::string& payload) {
        switch (op) {
            case opcode::ping:
                co_await send_pong(payload);
                break;
                
            case opcode::pong:
                // Handle pong (for ping timeout tracking)
                break;
                
            case opcode::close: {
                auto [code, reason] = parse_close_payload(payload);
                ELIO_LOG_DEBUG("WebSocket close received: {} {}", 
                              static_cast<uint16_t>(code), reason);
                
                if (state_ == connection_state::open) {
                    // Send close response
                    state_ = connection_state::closing;
                    auto frame = encode_close_frame(code, reason, !is_server_);
                    co_await send_raw(frame);
                }
                state_ = connection_state::closed;
                break;
            }
            
            default:
                break;
        }
    }
    
    stream_type stream_;
    connection_state state_ = connection_state::connecting;
    bool is_server_ = true;
    std::string subprotocol_;
    frame_parser parser_;
    std::vector<char> buffer_ = std::vector<char>(8192);
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
    
    // Decode and check key length (should be 16 bytes = 24 chars base64)
    if (ws_key.size() != 24) {
        result.error = "Invalid Sec-WebSocket-Key length";
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

/// WebSocket route for the HTTP router
struct ws_route {
    std::string pattern;
    std::regex regex;
    ws_handler_func handler;
    server_config config;
};

/// Extended HTTP router with WebSocket support
class ws_router : public router {
public:
    ws_router() = default;
    
    /// Add a WebSocket route
    void websocket(std::string_view pattern, ws_handler_func handler,
                   server_config config = {}) {
        ws_route route;
        route.pattern = pattern;
        route.handler = std::move(handler);
        route.config = config;
        
        // Convert pattern to regex (same as HTTP routes)
        std::string regex_str = "^";
        for (char c : pattern) {
            if (c == '*') {
                regex_str += ".*";
            } else if (c == '.' || c == '+' || c == '?' || c == '(' || c == ')' ||
                       c == '[' || c == ']' || c == '{' || c == '}' || c == '\\' ||
                       c == '^' || c == '$' || c == '|') {
                regex_str += '\\';
                regex_str += c;
            } else {
                regex_str += c;
            }
        }
        regex_str += "$";
        route.regex = std::regex(regex_str);
        
        ws_routes_.push_back(std::move(route));
    }
    
    /// Find matching WebSocket route
    const ws_route* find_ws_route(std::string_view path) const {
        for (const auto& route : ws_routes_) {
            std::cmatch match;
            if (std::regex_match(path.data(), path.data() + path.size(), 
                                match, route.regex)) {
                return &route;
            }
        }
        return nullptr;
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
    coro::task<void> listen(net::ipv4_address addr) {
        auto* sched = runtime::scheduler::current();
        if (!sched) {
            ELIO_LOG_ERROR("WebSocket server must be started from within a scheduler context");
            co_return;
        }

        auto listener_result = net::tcp_listener::bind(addr);
        if (!listener_result) {
            ELIO_LOG_ERROR("Failed to bind WebSocket server: {}", strerror(errno));
            co_return;
        }

        ELIO_LOG_INFO("WebSocket server listening on {}", addr.to_string());

        auto& listener = *listener_result;
        running_ = true;

        while (running_) {
            auto stream_result = co_await listener.accept();
            if (!stream_result) {
                if (running_) {
                    ELIO_LOG_ERROR("Accept error: {}", strerror(errno));
                }
                continue;
            }

            // Spawn connection handler
            auto handler = handle_connection(std::move(*stream_result));
            sched->spawn(handler.release());
        }
    }

    /// Start listening with TLS (HTTPS/WSS)
    coro::task<void> listen_tls(net::ipv4_address addr, tls::tls_context& tls_ctx) {
        auto* sched = runtime::scheduler::current();
        if (!sched) {
            ELIO_LOG_ERROR("Secure WebSocket server must be started from within a scheduler context");
            co_return;
        }

        auto listener_result = net::tcp_listener::bind(addr);
        if (!listener_result) {
            ELIO_LOG_ERROR("Failed to bind secure WebSocket server: {}", strerror(errno));
            co_return;
        }

        ELIO_LOG_INFO("Secure WebSocket server listening on {}", addr.to_string());

        auto& listener = *listener_result;
        running_ = true;

        while (running_) {
            auto stream_result = co_await listener.accept();
            if (!stream_result) {
                if (running_) {
                    ELIO_LOG_ERROR("Accept error: {}", strerror(errno));
                }
                continue;
            }

            // Spawn TLS connection handler
            auto handler = handle_tls_connection(std::move(*stream_result), tls_ctx);
            sched->spawn(handler.release());
        }
    }
    
    /// Stop the server
    void stop() {
        running_ = false;
    }
    
    /// Check if server is running
    bool is_running() const noexcept { return running_; }
    
private:
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
        auto hs_result = co_await stream.handshake();
        if (!hs_result) {
            ELIO_LOG_ERROR("TLS handshake failed for {}", client_addr);
            co_return;
        }
        
        co_await handle_request(stream, client_addr);
        
        co_await stream.shutdown();
    }
    
    /// Handle HTTP request (check for WebSocket upgrade)
    template<typename Stream>
    coro::task<void> handle_request(Stream& stream, const std::string& client_addr) {
        std::vector<char> buffer(http_config_.read_buffer_size);
        request_parser parser;
        
        // Read HTTP request
        while (!parser.is_complete() && !parser.has_error()) {
            auto result = co_await stream.read(buffer.data(), buffer.size());
            
            if (result.result <= 0) {
                co_return;
            }
            
            auto [parse_result, consumed] = parser.parse(
                std::string_view(buffer.data(), result.result));
            
            if (parse_result == parse_result::error) {
                co_return;
            }
        }
        
        if (parser.has_error()) {
            co_return;
        }
        
        auto req = request::from_parser(parser);
        
        // Check for WebSocket upgrade
        auto* ws_route = router_.find_ws_route(req.path());
        
        if (ws_route) {
            // Handle WebSocket upgrade
            auto upgrade = validate_upgrade_request(req, ws_route->config);
            
            if (!upgrade.success) {
                ELIO_LOG_WARNING("WebSocket upgrade failed: {}", upgrade.error);
                auto resp = response::bad_request(upgrade.error);
                co_await send_response(stream, resp);
                co_return;
            }
            
            // Send upgrade response
            auto ws_key = req.header("Sec-WebSocket-Key");
            auto resp = build_upgrade_response(ws_key, upgrade.accepted_protocol);
            co_await send_response(stream, resp);
            
            if (http_config_.enable_logging) {
                ELIO_LOG_INFO("WebSocket upgrade: {} from {}", req.path(), client_addr);
            }
            
            // Create WebSocket connection and run handler
            ws_connection conn(&stream);
            conn.set_open(upgrade.accepted_protocol);
            conn.set_max_message_size(ws_route->config.max_message_size);
            conn.set_buffer_size(ws_route->config.read_buffer_size);
            
            try {
                co_await ws_route->handler(conn);
            } catch (const std::exception& e) {
                ELIO_LOG_ERROR("WebSocket handler exception: {}", e.what());
            }
            
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
            
            co_await send_response(stream, resp);
        }
    }
    
    /// Send HTTP response
    template<typename Stream>
    coro::task<void> send_response(Stream& stream, const response& resp) {
        auto data = resp.serialize();
        
        size_t sent = 0;
        while (sent < data.size()) {
            auto result = co_await stream.write(data.data() + sent, data.size() - sent);
            if (result.result <= 0) {
                break;
            }
            sent += result.result;
        }
    }
    
    ws_router router_;
    http::server_config http_config_;
    std::atomic<bool> running_{false};
};

} // namespace elio::http::websocket
