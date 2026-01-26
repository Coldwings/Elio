#pragma once

/// @file rpc_server.hpp
/// @brief RPC server implementation
///
/// This module provides an async RPC server that supports:
/// - Method registration with type-safe handlers
/// - Concurrent client connections
/// - Request/response correlation
/// - Error handling and propagation
///
/// Usage:
/// @code
/// rpc_server<tcp_stream> server;
/// server.register_method<GetUserMethod>([](const GetUserRequest& req) 
///     -> coro::task<GetUserResponse> {
///     GetUserResponse resp;
///     resp.name = "John";
///     co_return resp;
/// });
/// co_await server.serve(listener);
/// @endcode

#include "rpc_protocol.hpp"

#include <elio/coro/task.hpp>
#include <elio/net/tcp.hpp>
#include <elio/net/uds.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/log/macros.hpp>

#include <memory>
#include <unordered_map>
#include <functional>
#include <atomic>

namespace elio::rpc {

// ============================================================================
// RPC Context
// ============================================================================

/// Cleanup callback type - invoked after response is sent
using cleanup_callback_t = std::function<void()>;

/// Context passed to RPC handlers
struct rpc_context {
    uint32_t request_id;                    ///< Request ID for correlation
    method_id_t method_id;                  ///< Method being called
    std::optional<uint32_t> timeout_ms;     ///< Client-specified timeout (if any)
    
    /// Check if request has a timeout
    bool has_timeout() const noexcept {
        return timeout_ms.has_value();
    }
};

// ============================================================================
// Handler types
// ============================================================================

/// Handler result containing success flag, payload, and optional cleanup callback
struct handler_result {
    bool success = false;
    buffer_writer payload;
    cleanup_callback_t cleanup;  ///< Optional cleanup callback (runs after response sent)
    
    handler_result() = default;
    handler_result(bool s, buffer_writer p, cleanup_callback_t c = nullptr)
        : success(s), payload(std::move(p)), cleanup(std::move(c)) {}
};

/// Type-erased handler that processes raw request data
using raw_handler_t = std::function<coro::task<handler_result>(
    const rpc_context& ctx,
    buffer_view payload
)>;

// ============================================================================
// RPC Session
// ============================================================================

/// A single client session on the server
template<typename Stream>
class rpc_session : public std::enable_shared_from_this<rpc_session<Stream>> {
public:
    using ptr = std::shared_ptr<rpc_session>;
    
    /// Create a new session
    static ptr create(Stream stream,
                      const std::unordered_map<method_id_t, raw_handler_t>& handlers)
    {
        return ptr(new rpc_session(std::move(stream), handlers));
    }
    
    /// Run the session (process requests until disconnect)
    coro::task<void> run() {
        auto self = this->shared_from_this();
        ELIO_LOG_DEBUG("RPC session started");
        
        while (stream_.is_valid() && !closed_.load(std::memory_order_acquire)) {
            auto frame = co_await read_frame(stream_);
            if (!frame) {
                ELIO_LOG_DEBUG("RPC session: connection closed");
                break;
            }
            
            auto& [header, payload] = *frame;
            
            switch (header.type) {
                case message_type::request:
                    co_await handle_request(header, std::move(payload));
                    break;
                    
                case message_type::ping: {
                    auto pong = build_pong(header.request_id);
                    buffer_writer empty;
                    co_await send_mutex_.lock();
                    sync::lock_guard guard(send_mutex_);
                    co_await write_frame(stream_, pong, empty);
                    break;
                }
                    
                case message_type::cancel:
                    // TODO: Support request cancellation
                    ELIO_LOG_DEBUG("RPC session: received cancel for request {}",
                                  header.request_id);
                    break;
                    
                default:
                    ELIO_LOG_WARNING("RPC session: unexpected message type {}",
                                    static_cast<int>(header.type));
                    break;
            }
        }
        
        closed_.store(true, std::memory_order_release);
        ELIO_LOG_DEBUG("RPC session ended");
    }
    
    /// Close the session
    void close() {
        closed_.store(true, std::memory_order_release);
    }
    
    /// Check if session is closed
    bool is_closed() const noexcept {
        return closed_.load(std::memory_order_acquire);
    }
    
private:
    rpc_session(Stream stream,
                const std::unordered_map<method_id_t, raw_handler_t>& handlers)
        : stream_(std::move(stream))
        , handlers_(handlers) {}
    
    /// Handle an incoming request
    coro::task<void> handle_request(const frame_header& header, message_buffer payload) {
        // Find handler
        auto it = handlers_.find(header.method_id);
        if (it == handlers_.end()) {
            ELIO_LOG_WARNING("RPC session: method {} not found", header.method_id);
            auto [err_header, err_payload] = build_error_response(
                header.request_id, 
                rpc_error::method_not_found,
                "Method not found"
            );
            co_await send_response(err_header, err_payload);
            co_return;
        }
        
        // Build context
        rpc_context ctx;
        ctx.request_id = header.request_id;
        ctx.method_id = header.method_id;
        
        // Parse timeout if present
        buffer_view view = payload.view();
        if (has_flag(header.flags, message_flags::has_timeout)) {
            ctx.timeout_ms = view.read<uint32_t>();
        }
        
        // Call handler and capture result or error
        bool handler_success = false;
        buffer_writer response_payload;
        cleanup_callback_t cleanup_cb;
        rpc_error error_code = rpc_error::success;
        std::string error_message;
        
        try {
            auto result = co_await it->second(ctx, view);
            handler_success = result.success;
            response_payload = std::move(result.payload);
            cleanup_cb = std::move(result.cleanup);
            if (!handler_success) {
                error_code = rpc_error::internal_error;
                error_message = "Handler failed";
            }
        } catch (const serialization_error& e) {
            ELIO_LOG_ERROR("RPC session: serialization error: {}", e.what());
            error_code = rpc_error::serialization_error;
            error_message = e.what();
        } catch (const std::exception& e) {
            ELIO_LOG_ERROR("RPC session: handler exception: {}", e.what());
            error_code = rpc_error::internal_error;
            error_message = e.what();
        }
        
        // Send response (outside exception handlers)
        if (handler_success) {
            frame_header resp_header;
            resp_header.request_id = header.request_id;
            resp_header.type = message_type::response;
            resp_header.flags = message_flags::none;
            resp_header.method_id = 0;
            resp_header.payload_length = static_cast<uint32_t>(response_payload.size());
            
            co_await send_response(resp_header, response_payload);
        } else {
            auto [err_header, err_payload] = build_error_response(
                header.request_id,
                error_code,
                error_message
            );
            co_await send_response(err_header, err_payload);
        }
        
        // Invoke cleanup callback after response is sent
        if (cleanup_cb) {
            try {
                cleanup_cb();
            } catch (const std::exception& e) {
                ELIO_LOG_ERROR("RPC session: cleanup callback exception: {}", e.what());
            }
        }
    }
    
    /// Send a response frame
    coro::task<void> send_response(const frame_header& header, 
                                    const buffer_writer& payload) 
    {
        co_await send_mutex_.lock();
        sync::lock_guard guard(send_mutex_);
        co_await write_frame(stream_, header, payload);
    }
    
    Stream stream_;
    const std::unordered_map<method_id_t, raw_handler_t>& handlers_;
    std::atomic<bool> closed_{false};
    sync::mutex send_mutex_;
};

// ============================================================================
// RPC Server
// ============================================================================

/// RPC server that accepts connections and handles requests
template<typename Stream>
class rpc_server {
public:
    using stream_type = Stream;
    using session_type = rpc_session<Stream>;
    using session_ptr = typename session_type::ptr;
    
    rpc_server() = default;
    ~rpc_server() = default;
    
    // Non-copyable, non-movable
    rpc_server(const rpc_server&) = delete;
    rpc_server& operator=(const rpc_server&) = delete;
    rpc_server(rpc_server&&) = delete;
    rpc_server& operator=(rpc_server&&) = delete;
    
    /// Register a method handler
    /// @tparam Method The method descriptor type
    /// @tparam Handler A callable returning task<Response>
    template<typename Method, typename Handler>
    void register_method(Handler handler) {
        using Request = typename Method::request_type;
        using Response = typename Method::response_type;
        
        raw_handler_t raw_handler = [h = std::move(handler)](
            [[maybe_unused]] const rpc_context& ctx,
            buffer_view payload
        ) -> coro::task<handler_result> {
            // Deserialize request
            Request request;
            deserialize(payload, request);
            
            // Call handler
            Response response = co_await h(request);
            
            // Serialize response
            buffer_writer response_data;
            serialize(response_data, response);
            
            co_return handler_result{true, std::move(response_data), nullptr};
        };
        
        handlers_[Method::id] = std::move(raw_handler);
    }
    
    /// Register a method handler with context
    /// @tparam Method The method descriptor type
    /// @tparam Handler A callable taking (ctx, request) and returning task<Response>
    template<typename Method, typename Handler>
    void register_method_with_context(Handler handler) {
        using Request = typename Method::request_type;
        using Response = typename Method::response_type;
        
        raw_handler_t raw_handler = [h = std::move(handler)](
            const rpc_context& ctx,
            buffer_view payload
        ) -> coro::task<handler_result> {
            // Deserialize request
            Request request;
            deserialize(payload, request);
            
            // Call handler with context
            Response response = co_await h(ctx, request);
            
            // Serialize response
            buffer_writer response_data;
            serialize(response_data, response);
            
            co_return handler_result{true, std::move(response_data), nullptr};
        };
        
        handlers_[Method::id] = std::move(raw_handler);
    }
    
    /// Register a synchronous method handler (doesn't need co_await)
    template<typename Method, typename Handler>
    void register_sync_method(Handler handler) {
        using Request = typename Method::request_type;
        using Response = typename Method::response_type;
        
        raw_handler_t raw_handler = [h = std::move(handler)](
            [[maybe_unused]] const rpc_context& ctx,
            buffer_view payload
        ) -> coro::task<handler_result> {
            // Deserialize request
            Request request;
            deserialize(payload, request);
            
            // Call synchronous handler
            Response response = h(request);
            
            // Serialize response
            buffer_writer response_data;
            serialize(response_data, response);
            
            co_return handler_result{true, std::move(response_data), nullptr};
        };
        
        handlers_[Method::id] = std::move(raw_handler);
    }
    
    /// Register a method handler with cleanup callback support
    /// Handler should return std::pair<Response, cleanup_callback_t>
    /// The cleanup callback is invoked after the response is successfully sent
    /// @tparam Method The method descriptor type
    /// @tparam Handler A callable returning task<std::pair<Response, cleanup_callback_t>>
    template<typename Method, typename Handler>
    void register_method_with_cleanup(Handler handler) {
        using Request = typename Method::request_type;
        
        raw_handler_t raw_handler = [h = std::move(handler)](
            [[maybe_unused]] const rpc_context& ctx,
            buffer_view payload
        ) -> coro::task<handler_result> {
            // Deserialize request
            Request request;
            deserialize(payload, request);
            
            // Call handler - returns (response, cleanup_callback)
            auto [response, cleanup] = co_await h(request);
            
            // Serialize response
            buffer_writer response_data;
            serialize(response_data, response);
            
            co_return handler_result{true, std::move(response_data), std::move(cleanup)};
        };
        
        handlers_[Method::id] = std::move(raw_handler);
    }
    
    /// Register a method handler with context and cleanup callback support
    /// Handler should return std::pair<Response, cleanup_callback_t>
    /// @tparam Method The method descriptor type
    /// @tparam Handler A callable taking (ctx, request) and returning task<std::pair<Response, cleanup_callback_t>>
    template<typename Method, typename Handler>
    void register_method_with_context_and_cleanup(Handler handler) {
        using Request = typename Method::request_type;
        
        raw_handler_t raw_handler = [h = std::move(handler)](
            const rpc_context& ctx,
            buffer_view payload
        ) -> coro::task<handler_result> {
            // Deserialize request
            Request request;
            deserialize(payload, request);
            
            // Call handler with context - returns (response, cleanup_callback)
            auto [response, cleanup] = co_await h(ctx, request);
            
            // Serialize response
            buffer_writer response_data;
            serialize(response_data, response);
            
            co_return handler_result{true, std::move(response_data), std::move(cleanup)};
        };
        
        handlers_[Method::id] = std::move(raw_handler);
    }
    
    /// Serve connections from a TCP listener
    coro::task<void> serve(net::tcp_listener& listener)
    requires std::is_same_v<Stream, net::tcp_stream>
    {
        ELIO_LOG_INFO("RPC server starting on {}",
                     listener.local_address().to_string());
        running_.store(true, std::memory_order_release);
        
        while (running_.load(std::memory_order_acquire)) {
            auto stream = co_await listener.accept();
            if (!stream) {
                if (running_.load(std::memory_order_acquire)) {
                    ELIO_LOG_ERROR("RPC server: accept failed");
                }
                continue;
            }
            
            // Create and start session
            auto session = session_type::create(std::move(*stream), handlers_);
            
            // Track session
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                sessions_.push_back(session);
            }
            
            // Spawn session handler
            auto* sched = runtime::scheduler::current();
            if (sched) {
                auto task = run_session(session);
                sched->spawn(task.release());
            }
        }
        
        ELIO_LOG_INFO("RPC server stopped");
    }
    
    /// Serve connections from a UDS listener
    coro::task<void> serve(net::uds_listener& listener)
    requires std::is_same_v<Stream, net::uds_stream>
    {
        ELIO_LOG_INFO("RPC server starting on {}",
                     listener.local_address().to_string());
        running_.store(true, std::memory_order_release);
        
        while (running_.load(std::memory_order_acquire)) {
            auto stream = co_await listener.accept();
            if (!stream) {
                if (running_.load(std::memory_order_acquire)) {
                    ELIO_LOG_ERROR("RPC server: accept failed");
                }
                continue;
            }
            
            // Create and start session
            auto session = session_type::create(std::move(*stream), handlers_);
            
            // Track session
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                sessions_.push_back(session);
            }
            
            // Spawn session handler
            auto* sched = runtime::scheduler::current();
            if (sched) {
                auto task = run_session(session);
                sched->spawn(task.release());
            }
        }
        
        ELIO_LOG_INFO("RPC server stopped");
    }
    
    /// Handle a single client stream (useful for testing or custom accept logic)
    coro::task<void> handle_client(Stream stream) {
        auto session = session_type::create(std::move(stream), handlers_);
        
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions_.push_back(session);
        }
        
        co_await session->run();
        
        // Remove from active sessions
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions_.erase(
                std::remove_if(sessions_.begin(), sessions_.end(),
                    [&](const session_ptr& s) { return s == session; }),
                sessions_.end()
            );
        }
    }
    
    /// Stop the server
    void stop() {
        running_.store(false, std::memory_order_release);
        
        // Close all sessions
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto& session : sessions_) {
            session->close();
        }
    }
    
    /// Check if server is running
    bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }
    
    /// Get number of active sessions
    size_t session_count() const {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        return sessions_.size();
    }
    
private:
    /// Run a session and clean up when done
    coro::task<void> run_session(session_ptr session) {
        co_await session->run();
        
        // Remove from active sessions
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions_.erase(
                std::remove_if(sessions_.begin(), sessions_.end(),
                    [&](const session_ptr& s) { return s == session; }),
                sessions_.end()
            );
        }
    }
    
    std::unordered_map<method_id_t, raw_handler_t> handlers_;
    std::atomic<bool> running_{false};
    
    mutable std::mutex sessions_mutex_;
    std::vector<session_ptr> sessions_;
};

/// Type alias for TCP RPC server
using tcp_rpc_server = rpc_server<net::tcp_stream>;

/// Type alias for UDS RPC server
using uds_rpc_server = rpc_server<net::uds_stream>;

} // namespace elio::rpc
