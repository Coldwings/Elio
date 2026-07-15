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
#include <elio/sync/primitives.hpp>
#include <elio/log/macros.hpp>

#include <string>
#include <string_view>
#include <cerrno>
#include <memory>
#include <optional>
#include <stdexcept>
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
        , tls_ctx_(tls::tls_mode::client)
        , send_mutex_(std::make_unique<sync::mutex>()) {
        // Setup TLS context using shared utility
        http::init_client_tls_context(tls_ctx_, config_.verify_certificate);

        parser_.set_max_message_size(config_.max_message_size);
        // Client endpoint: incoming server frames must not be masked
        // (RFC 6455 §5.1).
        parser_.set_role(endpoint_role::client);
        buffer_.resize(config_.read_buffer_size);
    }

    /// Destructor
    ~ws_client() = default;

    // Move only.  send_mutex_ is held via unique_ptr so ws_client remains
    // movable (ws_connect returns std::optional<ws_client>) while still
    // providing stable per-connection mutex addresses for waiters.
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

        auto frame = encode_close_frame(code, reason, true);
        state_ = connection_state::closing;

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
                errno = ECANCELED;
                co_return std::nullopt;
            }

            // Process already-parsed frames in wire order.
            while (parser_.next_frame_is_control_frame()) {
                auto control_frame = parser_.get_next_control_frame();
                if (!control_frame) {
                    break;
                }
                co_await handle_control_frame(control_frame->first, control_frame->second);
            }

            if (state_ == connection_state::closed) {
                co_return std::nullopt;
            }

            if (parser_.next_frame_is_message()) {
                co_return parser_.get_next_message();
            }

            // Check for errors (e.g. masked frame from server, or message
            // too large): RFC 6455 §5.1 requires emitting a close frame
            // with the appropriate code before tearing down.
            if (parser_.has_error()) {
                ELIO_LOG_ERROR("WebSocket parse error: {}", parser_.error());
                co_await fail_connection(parser_.error_close_code());
                co_return std::nullopt;
            }

            // Read more data
            auto result = co_await read(buffer_.data(), buffer_.size(), token);

            if (result.result <= 0) {
                if (result.result == -ECANCELED && token.is_cancelled()) {
                    errno = ECANCELED;
                    co_return std::nullopt;
                }
                if (result.result == 0) {
                    ELIO_LOG_DEBUG("WebSocket connection closed by server");
                } else {
                    ELIO_LOG_ERROR("WebSocket read error: {}", strerror(-result.result));
                }
                state_ = connection_state::closed;
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
    
    /// Internal connect implementation
    coro::task<bool> connect_impl(std::string_view url_str, coro::cancel_token token) {
        state_ = connection_state::connecting;

        // Check if already cancelled
        if (token.is_cancelled()) {
            errno = ECANCELED;
            state_ = connection_state::closed;
            co_return false;
        }
        
        // Parse URL
        auto parsed = parse_ws_url(url_str);
        if (!parsed) {
            ELIO_LOG_ERROR("Invalid WebSocket URL: {}", url_str);
            errno = EINVAL;
            state_ = connection_state::closed;
            co_return false;
        }

        if (!config_.user_agent.empty() &&
            !http::detail::is_valid_header_value(config_.user_agent)) {
            ELIO_LOG_ERROR("Invalid WebSocket User-Agent header value");
            errno = EINVAL;
            state_ = connection_state::closed;
            co_return false;
        }
        if (!config_.origin.empty() &&
            !http::detail::is_valid_header_value(config_.origin)) {
            ELIO_LOG_ERROR("Invalid WebSocket Origin header value");
            errno = EINVAL;
            state_ = connection_state::closed;
            co_return false;
        }
        for (const auto& protocol : config_.subprotocols) {
            if (!http::detail::is_valid_token(protocol)) {
                ELIO_LOG_ERROR("Invalid WebSocket subprotocol value");
                errno = EINVAL;
                state_ = connection_state::closed;
                co_return false;
            }
        }
        
        host_ = parsed->host;
        path_ = parsed->path.empty() ? "/" : parsed->path;
        secure_ = parsed->secure;
        host_authority_ = parsed->host_authority;
        
        uint16_t port = parsed->port;
        if (port == 0) {
            port = secure_ ? 443 : 80;
        }
        port_ = port;
        
        ELIO_LOG_DEBUG("Connecting to WebSocket server {}:{}{}", host_, port, path_);
        
        // Check cancellation before connection
        if (token.is_cancelled()) {
            errno = ECANCELED;
            state_ = connection_state::closed;
            co_return false;
        }
        
        // Establish connection using shared utility
        auto conn_result = co_await http::client_connect(
            host_,
            port,
            secure_,
            &tls_ctx_,
            config_.resolve_options,
            config_.rotate_resolved_addresses,
            config_.connect_timeout,
            token);
        if (!conn_result) {
            state_ = connection_state::closed;
            co_return false;
        }
        stream_ = std::move(*conn_result);
        
        // Check cancellation before handshake
        if (token.is_cancelled()) {
            http::detail::abort_stream_io(stream_);
            errno = ECANCELED;
            stream_.disconnect();
            state_ = connection_state::closed;
            co_return false;
        }
        
        // Perform WebSocket handshake
        bool success = co_await perform_handshake(std::move(token));
        if (success) {
            state_ = connection_state::open;
            ELIO_LOG_DEBUG("WebSocket connected to {}{}", host_, path_);
        } else {
            int saved_errno = errno;
            stream_.disconnect();
            errno = saved_errno;
            state_ = connection_state::closed;
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
        std::string host_authority;
    };
    
    /// Parse WebSocket URL
    static std::optional<ws_url> parse_ws_url(std::string_view url_str) {
        std::string normalized;
        if (url_str.starts_with("wss://")) {
            normalized = "https://";
            url_str = url_str.substr(6);
        } else if (url_str.starts_with("ws://")) {
            normalized = "http://";
            url_str = url_str.substr(5);
        } else {
            return std::nullopt;
        }

        normalized.append(url_str);
        auto parsed = http::url::parse(normalized);
        if (!parsed) {
            return std::nullopt;
        }

        if (parsed->scheme != "http" && parsed->scheme != "https") {
            return std::nullopt;
        }
        if (!parsed->fragment.empty()) {
            return std::nullopt;
        }

        ws_url result;
        result.host = parsed->host;
        result.port = parsed->port;
        result.path = parsed->path_with_query();
        result.secure = parsed->is_secure();
        result.host_authority = parsed->host_authority();
        return result;
    }
    
    /// Perform WebSocket upgrade handshake
    coro::task<bool> perform_handshake(coro::cancel_token token) {
        // Generate key
        ws_key_ = generate_websocket_key();
        
        std::string request;
        try {
            // Build handshake request
            request = build_client_handshake(host_authority_, path_, ws_key_,
                                             config_.subprotocols,
                                             config_.origin);

            // Add User-Agent if configured
            if (!config_.user_agent.empty()) {
                if (!http::detail::is_valid_header_value(config_.user_agent)) {
                    throw std::invalid_argument(
                        "elio::http::websocket: invalid User-Agent header value");
                }

                // Insert before final \r\n
                size_t pos = request.rfind("\r\n\r\n");
                if (pos != std::string::npos) {
                    request.insert(pos, "\r\nUser-Agent: " + config_.user_agent);
                }
            }
        } catch (const std::invalid_argument& ex) {
            ELIO_LOG_ERROR("Invalid WebSocket handshake request: {}",
                           ex.what());
            errno = EINVAL;
            co_return false;
        }
        
        ELIO_LOG_DEBUG("Sending WebSocket handshake");
        
        // Send handshake
        auto send_result = co_await write_exactly(request.data(), request.size(),
                                                  token);
        if (send_result.result != static_cast<ssize_t>(request.size())) {
            if (send_result.result == -ECANCELED && token.is_cancelled()) {
                http::detail::abort_stream_io(stream_);
                errno = ECANCELED;
                co_return false;
            }
            ELIO_LOG_ERROR("Failed to send WebSocket handshake");
            errno = send_result.result == 0 ? ECONNRESET : -send_result.result;
            co_return false;
        }
        
        // Read and incrementally parse the upgrade response.  We feed bytes
        // straight into a response_parser so that anything pipelined behind
        // the headers (a server-pushed first frame in the same TCP segment,
        // uncommon but legal) can be reclaimed via take_remaining() and
        // handed off to the WebSocket frame parser instead of being
        // discarded.
        response_parser parser;
        parser.set_max_headers(config_.max_headers);
        parser.set_max_header_size(config_.max_header_size);
        size_t total_read = 0;
        auto* sched = runtime::scheduler::current();
        const bool deadline_enforced =
            sched != nullptr && config_.read_timeout.count() > 0;
        const auto response_deadline =
            std::chrono::steady_clock::now() + config_.read_timeout;
        while (!parser.is_complete() && !parser.has_error()) {
            if (token.is_cancelled()) {
                errno = ECANCELED;
                stream_.disconnect();
                co_return false;
            }

            io::io_result read_result{};
            if (deadline_enforced) {
                auto remaining =
                    response_deadline - std::chrono::steady_clock::now();
                if (remaining.count() <= 0) {
                    ELIO_LOG_ERROR("WebSocket handshake response timed out after {}s",
                                   config_.read_timeout.count());
                    errno = ETIMEDOUT;
                    stream_.disconnect();
                    co_return false;
                }

                auto timed_out = std::make_shared<std::atomic<bool>>(false);
                coro::cancel_source watchdog_cancel;
                auto watchdog = http::detail::arm_fd_shutdown_watchdog(
                    sched, stream_.fd(), remaining,
                    watchdog_cancel.get_token(), timed_out);
                read_result = co_await read(buffer_.data(), buffer_.size(),
                                            token);
                watchdog_cancel.cancel();
                co_await watchdog;
                if (timed_out->load(std::memory_order_acquire)) {
                    stream_.mark_externally_shut_down();
                    ELIO_LOG_ERROR("WebSocket handshake response timed out after {}s",
                                   config_.read_timeout.count());
                    errno = ETIMEDOUT;
                    stream_.disconnect();
                    co_return false;
                }
            } else {
                read_result = co_await read(buffer_.data(), buffer_.size(),
                                            token);
            }

            if (read_result.result <= 0) {
                if (read_result.result == -ECANCELED && token.is_cancelled()) {
                    http::detail::abort_stream_io(stream_);
                    errno = ECANCELED;
                    co_return false;
                }
                ELIO_LOG_ERROR("Failed to read WebSocket handshake response");
                errno = read_result.result == 0 ? ECONNRESET : -read_result.result;
                co_return false;
            }

            auto [pres, consumed] = parser.parse(
                std::string_view(buffer_.data(), static_cast<size_t>(read_result.result)));
            (void)consumed;

            if (pres == parse_result::error) {
                ELIO_LOG_ERROR("Failed to parse WebSocket handshake response");
                errno = EBADMSG;
                co_return false;
            }

            total_read += static_cast<size_t>(read_result.result);
            if (total_read > 8192 && !parser.is_complete()) {
                ELIO_LOG_ERROR("WebSocket handshake response too large");
                errno = EMSGSIZE;
                co_return false;
            }
        }

        if (parser.has_error()) {
            ELIO_LOG_ERROR("Failed to parse WebSocket handshake response");
            errno = EBADMSG;
            co_return false;
        }

        // Check status code
        if (parser.get_status() != status::switching_protocols) {
            ELIO_LOG_ERROR("WebSocket handshake failed: {}",
                          static_cast<int>(parser.get_status()));
            errno = EBADMSG;
            co_return false;
        }

        auto upgrade = parser.get_headers().get("Upgrade");
        if (!detail::header_has_token(upgrade, "websocket")) {
            ELIO_LOG_ERROR("WebSocket handshake missing Upgrade: websocket");
            errno = EBADMSG;
            co_return false;
        }

        auto connection = parser.get_headers().get("Connection");
        if (!detail::header_has_token(connection, "upgrade")) {
            ELIO_LOG_ERROR("WebSocket handshake missing Connection: Upgrade");
            errno = EBADMSG;
            co_return false;
        }

        // Verify Sec-WebSocket-Accept
        auto accept = parser.get_headers().get("Sec-WebSocket-Accept");
        if (!verify_websocket_accept(accept, ws_key_)) {
            ELIO_LOG_ERROR("Invalid Sec-WebSocket-Accept header");
            errno = EBADMSG;
            co_return false;
        }

        // Get negotiated protocol
        auto protocol = parser.get_headers().get("Sec-WebSocket-Protocol");
        subprotocol_.clear();
        if (!protocol.empty()) {
            auto selected = http::detail::trim_ows(protocol);
            bool offered = false;
            for (const auto& offered_protocol : config_.subprotocols) {
                if (selected == offered_protocol) {
                    offered = true;
                    break;
                }
            }
            if (!offered) {
                ELIO_LOG_ERROR("Server selected unoffered WebSocket subprotocol: {}",
                               protocol);
                errno = EBADMSG;
                co_return false;
            }
            subprotocol_ = selected;
        }

        // Recover any bytes the server pipelined behind the 101 response so
        // the very first WebSocket frame is recognized.
        std::string pipelined = parser.take_remaining();
        if (!pipelined.empty()) {
            parser_.parse(reinterpret_cast<const uint8_t*>(pipelined.data()),
                          pipelined.size());
        }

        ELIO_LOG_DEBUG("WebSocket handshake successful");
        co_return true;
    }
    
    coro::task<io::io_result> read(void* buf, size_t len) {
        co_return co_await stream_.read(buf, len);
    }

    coro::task<io::io_result> read(void* buf, size_t len,
                                   coro::cancel_token token) {
        co_return co_await stream_.read(buf, len, std::move(token));
    }

    coro::task<io::io_result> write(const void* buf, size_t len) {
        co_return co_await stream_.write(buf, len);
    }

    coro::task<io::io_result> write_exactly(const void* buf, size_t len) {
        co_return co_await stream_.write_exactly(buf, len);
    }

    coro::task<io::io_result> write_exactly(const void* buf, size_t len,
                                            coro::cancel_token token) {
        co_return co_await stream_.write_exactly(buf, len, std::move(token));
    }
    
    /// Serialize an entire frame onto the wire under a per-connection mutex
    /// so that two coroutines calling send_text/send_binary/etc concurrently
    /// cannot byte-interleave their frames.  The receive loop's auto-pong /
    /// close-response paths also funnel through here and contend for the
    /// same lock, so this MUST NOT be invoked recursively from a write
    /// path that already holds the lock.
    coro::task<bool> send_raw(const std::vector<uint8_t>& data) {
        co_await send_mutex_->lock();
        sync::lock_guard send_guard(*send_mutex_);
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

    /// Emit a close frame with the given code and tear the connection down.
    coro::task<void> fail_connection(close_code code) {
        if (state_ == connection_state::open) {
            state_ = connection_state::closing;
            auto frame = encode_close_frame(code, "", true);
            co_await send_raw(frame);
        }
        state_ = connection_state::closed;
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
                auto close_info = parse_close_payload(payload);
                auto code = close_info.first;
                [[maybe_unused]] auto& reason = close_info.second;
                ELIO_LOG_DEBUG("WebSocket close received: {} {}",
                              static_cast<uint16_t>(code), reason);

                if (state_ == connection_state::open) {
                    // Send close response.  RFC 6455 §7.4.1 forbids sending
                    // no_status (1005) on the wire; map it to normal (1000).
                    state_ = connection_state::closing;
                    auto resp_code = (code == close_code::no_status)
                                         ? close_code::normal
                                         : code;
                    auto frame = encode_close_frame(resp_code, "", true);
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
    std::string host_authority_;
    std::string path_;
    uint16_t port_ = 0;
    bool secure_ = false;
    std::string ws_key_;
    std::string subprotocol_;
    
    connection_state state_ = connection_state::connecting;
    frame_parser parser_;
    std::vector<char> buffer_;
    std::unique_ptr<sync::mutex> send_mutex_;  ///< Serializes frame writes.
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
