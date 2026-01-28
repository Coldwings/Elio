#pragma once

/// @file websocket_client.hpp
/// @brief WebSocket client implementation for Elio
///
/// This file provides WebSocket client functionality including:
/// - Connection to WebSocket servers (ws:// and wss://)
/// - Automatic upgrade handshake
/// - Message send/receive with async I/O
/// - Ping/pong heartbeat support
/// - Reconnection support

#include <elio/http/websocket_frame.hpp>
#include <elio/http/websocket_handshake.hpp>
#include <elio/http/http_common.hpp>
#include <elio/http/http_parser.hpp>
#include <elio/http/client_base.hpp>
#include <elio/net/stream.hpp>
#include <elio/io/io_context.hpp>
#include <elio/coro/task.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/log/macros.hpp>

#include <string>
#include <string_view>
#include <memory>
#include <optional>
#include <vector>

namespace elio::http::websocket {

/// WebSocket client configuration
struct client_config : http::base_client_config {
    size_t max_message_size = 16 * 1024 * 1024;   ///< Max message size (16MB)
    std::vector<std::string> subprotocols;        ///< Requested subprotocols
    std::string origin;                           ///< Origin header (for browser compatibility)

    client_config() {
        user_agent = "elio-websocket/1.0";
    }
};

/// WebSocket client connection
class ws_client {
public:
    /// Create WebSocket client with default configuration
    ws_client() : ws_client(client_config{}) {}

    /// Create WebSocket client with configuration
    explicit ws_client(client_config config)
        : config_(config)
        , tls_ctx_(tls::tls_mode::client) {
        // Setup TLS context using shared utility
        http::init_client_tls_context(tls_ctx_, config_.verify_certificate);

        parser_.set_max_message_size(config_.max_message_size);
        buffer_.resize(config_.read_buffer_size);
    }
    
    /// Destructor
    ~ws_client() = default;
    
    // Move only
    ws_client(ws_client&&) = default;
    ws_client& operator=(ws_client&&) = default;
    ws_client(const ws_client&) = delete;
    ws_client& operator=(const ws_client&) = delete;
    
    /// Connect to a WebSocket server
    /// @param url WebSocket URL (ws:// or wss://)
    /// @return true on success, false on failure
    coro::task<bool> connect(std::string_view url_str) {
        return connect_impl(url_str, coro::cancel_token{});
    }
    
    /// Connect to a WebSocket server with cancellation support
    /// @param url WebSocket URL (ws:// or wss://)
    /// @param token Cancellation token
    /// @return true on success, false on failure
    coro::task<bool> connect(std::string_view url_str, coro::cancel_token token) {
        return connect_impl(url_str, std::move(token));
    }
    
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
        // Client frames must be masked
        auto frame = encode_text_frame(message, true);
        co_return co_await send_raw(frame);
    }
    
    /// Send a binary message
    coro::task<bool> send_binary(std::string_view data) {
        if (state_ != connection_state::open) {
            co_return false;
        }
        auto frame = encode_binary_frame(data, true);
        co_return co_await send_raw(frame);
    }
    
    /// Send a ping
    coro::task<bool> send_ping(std::string_view payload = "") {
        if (state_ != connection_state::open) {
            co_return false;
        }
        auto frame = encode_ping_frame(payload, true);
        co_return co_await send_raw(frame);
    }
    
    /// Send a pong
    coro::task<bool> send_pong(std::string_view payload = "") {
        if (state_ != connection_state::open) {
            co_return false;
        }
        auto frame = encode_pong_frame(payload, true);
        co_return co_await send_raw(frame);
    }
    
    /// Close the connection
    coro::task<void> close(close_code code = close_code::normal,
                          std::string_view reason = "") {
        if (state_ != connection_state::open) {
            co_return;
        }

        state_ = connection_state::closing;

        auto frame = encode_close_frame(code, reason, true);
        co_await send_raw(frame);

        // Wait for close response (with timeout)
        // Simplified: just mark as closed
        state_ = connection_state::closed;

        // Cleanup stream using unified close
        co_await stream_.close();
    }
    
    /// Receive next message (blocks until message available or connection closed)
    coro::task<std::optional<message>> receive() {
        return receive_impl(coro::cancel_token{});
    }
    
    /// Receive next message with cancellation support
    /// @param token Cancellation token
    /// @return Message on success, std::nullopt on close/error/cancel
    coro::task<std::optional<message>> receive(coro::cancel_token token) {
        return receive_impl(std::move(token));
    }
    
private:
    /// Internal receive implementation
    coro::task<std::optional<message>> receive_impl(coro::cancel_token token) {
        while (state_ == connection_state::open || state_ == connection_state::closing) {
            // Check for cancellation
            if (token.is_cancelled()) {
                co_return std::nullopt;
            }
            
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
                    ELIO_LOG_DEBUG("WebSocket connection closed by server");
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
    
    /// Internal connect implementation
    coro::task<bool> connect_impl(std::string_view url_str, coro::cancel_token token) {
        // Check if already cancelled
        if (token.is_cancelled()) {
            co_return false;
        }
        
        // Parse URL
        auto parsed = parse_ws_url(url_str);
        if (!parsed) {
            ELIO_LOG_ERROR("Invalid WebSocket URL: {}", url_str);
            co_return false;
        }
        
        host_ = parsed->host;
        path_ = parsed->path.empty() ? "/" : parsed->path;
        secure_ = parsed->secure;
        
        uint16_t port = parsed->port;
        if (port == 0) {
            port = secure_ ? 443 : 80;
        }
        
        ELIO_LOG_DEBUG("Connecting to WebSocket server {}:{}{}", host_, port, path_);
        
        // Check cancellation before connection
        if (token.is_cancelled()) {
            co_return false;
        }
        
        // Establish connection using shared utility
        auto conn_result = co_await http::client_connect(host_, port, secure_, &tls_ctx_);
        if (!conn_result) {
            co_return false;
        }
        stream_ = std::move(*conn_result);
        
        // Check cancellation before handshake
        if (token.is_cancelled()) {
            stream_.disconnect();
            co_return false;
        }
        
        // Perform WebSocket handshake
        bool success = co_await perform_handshake();
        if (success) {
            state_ = connection_state::open;
            ELIO_LOG_DEBUG("WebSocket connected to {}{}", host_, path_);
        }
        
        co_return success;
    }
    
public:
    
    /// Get TLS context for configuration
    tls::tls_context& tls_context() noexcept { return tls_ctx_; }
    
    /// Get configuration
    client_config& config() noexcept { return config_; }
    const client_config& config() const noexcept { return config_; }
    
private:
    /// WebSocket URL components
    struct ws_url {
        std::string host;
        uint16_t port = 0;
        std::string path;
        bool secure = false;
    };
    
    /// Parse WebSocket URL
    static std::optional<ws_url> parse_ws_url(std::string_view url_str) {
        ws_url result;
        
        // Check scheme
        if (url_str.starts_with("wss://")) {
            result.secure = true;
            url_str = url_str.substr(6);
        } else if (url_str.starts_with("ws://")) {
            result.secure = false;
            url_str = url_str.substr(5);
        } else if (url_str.starts_with("https://")) {
            result.secure = true;
            url_str = url_str.substr(8);
        } else if (url_str.starts_with("http://")) {
            result.secure = false;
            url_str = url_str.substr(7);
        } else {
            return std::nullopt;
        }
        
        // Find path
        auto path_pos = url_str.find('/');
        if (path_pos != std::string_view::npos) {
            result.path = url_str.substr(path_pos);
            url_str = url_str.substr(0, path_pos);
        } else {
            result.path = "/";
        }
        
        // Parse host:port
        auto colon_pos = url_str.rfind(':');
        if (colon_pos != std::string_view::npos) {
            result.host = url_str.substr(0, colon_pos);
            auto port_str = url_str.substr(colon_pos + 1);
            uint16_t port = 0;
            auto [ptr, ec] = std::from_chars(port_str.data(), 
                                              port_str.data() + port_str.size(), 
                                              port);
            if (ec == std::errc{}) {
                result.port = port;
            }
        } else {
            result.host = url_str;
        }
        
        if (result.host.empty()) {
            return std::nullopt;
        }
        
        return result;
    }
    
    /// Perform WebSocket upgrade handshake
    coro::task<bool> perform_handshake() {
        // Generate key
        ws_key_ = generate_websocket_key();
        
        // Build handshake request
        std::string authority = host_;
        if ((secure_ && port_ != 443) || (!secure_ && port_ != 80)) {
            authority += ":" + std::to_string(port_);
        }
        
        auto request = build_client_handshake(authority, path_, ws_key_,
                                               config_.subprotocols, config_.origin);
        
        // Add User-Agent if configured
        if (!config_.user_agent.empty()) {
            // Insert before final \r\n
            size_t pos = request.rfind("\r\n\r\n");
            if (pos != std::string::npos) {
                request.insert(pos, "\r\nUser-Agent: " + config_.user_agent);
            }
        }
        
        ELIO_LOG_DEBUG("Sending WebSocket handshake");
        
        // Send handshake
        auto send_result = co_await write(request.data(), request.size());
        if (send_result.result <= 0) {
            ELIO_LOG_ERROR("Failed to send WebSocket handshake");
            co_return false;
        }
        
        // Read response
        std::string response_data;
        response_data.reserve(1024);
        
        while (true) {
            auto read_result = co_await read(buffer_.data(), buffer_.size());
            if (read_result.result <= 0) {
                ELIO_LOG_ERROR("Failed to read WebSocket handshake response");
                co_return false;
            }
            
            response_data.append(buffer_.data(), static_cast<size_t>(read_result.result));
            
            // Check if we have complete headers
            if (response_data.find("\r\n\r\n") != std::string::npos) {
                break;
            }
            
            if (response_data.size() > 8192) {
                ELIO_LOG_ERROR("WebSocket handshake response too large");
                co_return false;
            }
        }
        
        // Parse response
        response_parser parser;
        auto [result, consumed] = parser.parse(response_data);
        
        if (result == parse_result::error) {
            ELIO_LOG_ERROR("Failed to parse WebSocket handshake response");
            co_return false;
        }
        
        // Check status code
        if (parser.get_status() != status::switching_protocols) {
            ELIO_LOG_ERROR("WebSocket handshake failed: {}", 
                          static_cast<int>(parser.get_status()));
            co_return false;
        }
        
        // Verify Sec-WebSocket-Accept
        auto accept = parser.get_headers().get("Sec-WebSocket-Accept");
        if (!verify_websocket_accept(accept, ws_key_)) {
            ELIO_LOG_ERROR("Invalid Sec-WebSocket-Accept header");
            co_return false;
        }
        
        // Get negotiated protocol
        auto protocol = parser.get_headers().get("Sec-WebSocket-Protocol");
        if (!protocol.empty()) {
            subprotocol_ = protocol;
        }
        
        ELIO_LOG_DEBUG("WebSocket handshake successful");
        co_return true;
    }
    
    coro::task<io::io_result> read(void* buf, size_t len) {
        co_return co_await stream_.read(buf, len);
    }

    coro::task<io::io_result> write(const void* buf, size_t len) {
        co_return co_await stream_.write(buf, len);
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
                ELIO_LOG_DEBUG("Received pong");
                break;
                
            case opcode::close: {
                auto [code, reason] = parse_close_payload(payload);
                ELIO_LOG_DEBUG("WebSocket close received: {} {}", 
                              static_cast<uint16_t>(code), reason);
                
                if (state_ == connection_state::open) {
                    // Send close response
                    state_ = connection_state::closing;
                    auto frame = encode_close_frame(code, reason, true);
                    co_await send_raw(frame);
                }
                state_ = connection_state::closed;
                break;
            }
            
            default:
                break;
        }
    }
    
    client_config config_;
    tls::tls_context tls_ctx_;
    net::stream stream_;
    
    std::string host_;
    std::string path_;
    uint16_t port_ = 0;
    bool secure_ = false;
    std::string ws_key_;
    std::string subprotocol_;
    
    connection_state state_ = connection_state::connecting;
    frame_parser parser_;
    std::vector<char> buffer_;
};

/// Convenience function for one-off WebSocket connection
inline coro::task<std::optional<ws_client>>
ws_connect(std::string_view url, client_config config = {}) {
    auto client = std::make_optional<ws_client>(config);
    bool success = co_await client->connect(url);
    if (!success) {
        co_return std::nullopt;
    }
    co_return client;
}

} // namespace elio::http::websocket
