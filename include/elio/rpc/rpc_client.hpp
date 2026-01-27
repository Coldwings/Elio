#pragma once

/// @file rpc_client.hpp
/// @brief RPC client implementation with out-of-order call support
///
/// This module provides an async RPC client that supports:
/// - Out-of-order responses using request ID correlation
/// - Per-call timeouts
/// - Background receive loop for response dispatching
/// - Automatic reconnection (optional)
///
/// Usage:
/// @code
/// auto client = co_await rpc_client<tcp_stream>::connect(ctx, addr);
/// auto result = co_await client->call<MyMethod>(request, 5000ms);
/// if (result.ok()) {
///     process(result.value());
/// }
/// @endcode

#include "rpc_protocol.hpp"

#include <elio/coro/task.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/time/timer.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/log/macros.hpp>

#include <memory>
#include <unordered_map>
#include <chrono>
#include <functional>

namespace elio::rpc {

// ============================================================================
// Pending request tracking
// ============================================================================

/// State for a pending RPC request
struct pending_request {
    sync::event completion_event;
    message_buffer response_data;
    frame_header response_header;
    rpc_error error = rpc_error::success;
    std::atomic<bool> completed{false};
    bool timed_out = false;
    
    pending_request() = default;
    
    /// Try to complete the request. Returns true if this call completed it.
    bool try_complete() noexcept {
        bool expected = false;
        return completed.compare_exchange_strong(expected, true, 
            std::memory_order_acq_rel);
    }
    
    /// Check if completed (read-only)
    bool is_completed() const noexcept {
        return completed.load(std::memory_order_acquire);
    }
};

// ============================================================================
// RPC Client
// ============================================================================

/// RPC client for making remote procedure calls
/// @tparam Stream The underlying stream type (tcp_stream or uds_stream)
template<typename Stream>
class rpc_client : public std::enable_shared_from_this<rpc_client<Stream>> {
public:
    using stream_type = Stream;
    using ptr = std::shared_ptr<rpc_client>;
    
    /// Create a new RPC client from an existing stream
    static ptr create(Stream stream) {
        return ptr(new rpc_client(std::move(stream)));
    }
    
    /// Connect to a TCP server and create client
    template<typename... Args>
    static coro::task<std::optional<ptr>> connect(
        io::io_context& ctx, 
        Args&&... args)
    requires std::is_same_v<Stream, net::tcp_stream>
    {
        auto stream = co_await net::tcp_connect(ctx, std::forward<Args>(args)...);
        if (!stream) {
            co_return std::nullopt;
        }
        auto client = create(std::move(*stream));
        client->start_receive_loop();
        co_return client;
    }
    
    /// Connect to a UDS server and create client
    template<typename... Args>
    static coro::task<std::optional<ptr>> connect(
        io::io_context& ctx,
        Args&&... args)
    requires std::is_same_v<Stream, net::uds_stream>
    {
        auto stream = co_await net::uds_connect(ctx, std::forward<Args>(args)...);
        if (!stream) {
            co_return std::nullopt;
        }
        auto client = create(std::move(*stream));
        client->start_receive_loop();
        co_return client;
    }
    
    /// Destructor
    ~rpc_client() {
        close();
    }
    
    // Non-copyable
    rpc_client(const rpc_client&) = delete;
    rpc_client& operator=(const rpc_client&) = delete;
    
    /// Check if client is connected
    bool is_connected() const noexcept {
        return stream_.is_valid() && !closed_.load(std::memory_order_acquire);
    }
    
    /// Close the client connection
    void close() {
        if (closed_.exchange(true, std::memory_order_acq_rel)) {
            return;  // Already closed
        }
        
        // Cancel all pending requests
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            for (auto& [id, req] : pending_requests_) {
                if (req->try_complete()) {
                    req->error = rpc_error::connection_closed;
                    req->completion_event.set();
                }
            }
            pending_requests_.clear();
        }
    }
    
    /// Make an RPC call with default timeout
    /// @tparam Method The method descriptor type
    /// @param request The request object
    /// @return Result containing response or error
    template<typename Method>
    coro::task<rpc_result<typename Method::response_type>> call(
        const typename Method::request_type& request)
    {
        return call<Method>(request, std::chrono::milliseconds(default_timeout_ms));
    }
    
    /// Make an RPC call with specified timeout
    /// @tparam Method The method descriptor type
    /// @param request The request object
    /// @param timeout Per-call timeout duration
    /// @return Result containing response or error
    template<typename Method, typename Rep, typename Period>
    coro::task<rpc_result<typename Method::response_type>> call(
        const typename Method::request_type& request,
        std::chrono::duration<Rep, Period> timeout)
    {
        return call_impl<Method>(request, timeout, coro::cancel_token{});
    }
    
    /// Make an RPC call with cancellation support
    /// @tparam Method The method descriptor type
    /// @param request The request object
    /// @param token Cancellation token
    /// @return Result containing response or error (rpc_error::cancelled if cancelled)
    template<typename Method>
    coro::task<rpc_result<typename Method::response_type>> call(
        const typename Method::request_type& request,
        coro::cancel_token token)
    {
        return call_impl<Method>(request, std::chrono::milliseconds(default_timeout_ms), std::move(token));
    }
    
    /// Make an RPC call with timeout and cancellation support
    /// @tparam Method The method descriptor type
    /// @param request The request object
    /// @param timeout Per-call timeout duration
    /// @param token Cancellation token
    /// @return Result containing response or error
    template<typename Method, typename Rep, typename Period>
    coro::task<rpc_result<typename Method::response_type>> call(
        const typename Method::request_type& request,
        std::chrono::duration<Rep, Period> timeout,
        coro::cancel_token token)
    {
        return call_impl<Method>(request, timeout, std::move(token));
    }
    
private:
    /// Internal implementation of call with cancellation support
    template<typename Method, typename Rep, typename Period>
    coro::task<rpc_result<typename Method::response_type>> call_impl(
        const typename Method::request_type& request,
        std::chrono::duration<Rep, Period> timeout,
        coro::cancel_token token)
    {
        using Response = typename Method::response_type;
        
        // Check if already cancelled
        if (token.is_cancelled()) {
            co_return rpc_result<Response>(rpc_error::cancelled);
        }
        
        if (!is_connected()) {
            co_return rpc_result<Response>(rpc_error::connection_closed);
        }
        
        // Generate request ID and create pending request
        uint32_t request_id = id_generator_.next();
        auto pending = std::make_shared<pending_request>();
        
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_[request_id] = pending;
        }
        
        // Register cancellation callback
        auto cancel_registration = token.on_cancel([this, pending, request_id]() {
            if (pending->try_complete()) {
                pending->error = rpc_error::cancelled;
                pending->completion_event.set();
            }
        });
        
        // Build and send request
        auto timeout_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
        auto [header, payload] = build_request(request_id, Method::id, request, timeout_ms);
        
        {
            co_await send_mutex_.lock();
            sync::lock_guard send_guard(send_mutex_);
            
            bool sent = co_await write_frame(stream_, header, payload);
            if (!sent) {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                pending_requests_.erase(request_id);
                co_return rpc_result<Response>(rpc_error::connection_closed);
            }
        }
        
        // Wait for response with timeout
        // Start a timeout coroutine
        auto self = this->shared_from_this();
        auto timeout_task = [](std::chrono::milliseconds ms,
                               std::shared_ptr<pending_request> pending,
                               coro::cancel_token tok) 
            -> coro::task<void> 
        {
            auto result = co_await time::sleep_for(ms, tok);
            
            // Only timeout if sleep completed normally (not cancelled)
            if (result == coro::cancel_result::completed && pending->try_complete()) {
                pending->timed_out = true;
                pending->error = rpc_error::timeout;
                pending->completion_event.set();
            }
        };
        
        // Spawn timeout watcher
        auto* sched = runtime::scheduler::current();
        if (sched) {
            auto task = timeout_task(
                std::chrono::duration_cast<std::chrono::milliseconds>(timeout), pending, token);
            sched->spawn(task.release());
        }
        
        // Wait for completion (either response, timeout, or cancellation)
        co_await pending->completion_event.wait();
        
        // Unregister cancellation callback
        cancel_registration.unregister();
        
        // Remove from pending
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_.erase(request_id);
        }
        
        // Check result
        if (pending->error != rpc_error::success) {
            co_return rpc_result<Response>(pending->error);
        }
        
        // Parse response
        try {
            if (pending->response_header.type == message_type::error) {
                buffer_view view = pending->response_data.view();
                auto err = parse_error(view);
                co_return rpc_result<Response>(err.code);
            }
            
            buffer_view view = pending->response_data.view();
            Response response = parse_response<Response>(view);
            co_return rpc_result<Response>(std::move(response));
        } catch (const serialization_error& e) {
            ELIO_LOG_ERROR("Failed to deserialize response: {}", e.what());
            co_return rpc_result<Response>(rpc_error::serialization_error);
        }
    }
    
public:
    /// Send a one-way message (no response expected)
    template<typename Method>
    coro::task<bool> send_oneway(const typename Method::request_type& request) {
        if (!is_connected()) {
            co_return false;
        }
        
        uint32_t request_id = id_generator_.next();
        auto [header, payload] = build_request(request_id, Method::id, request);
        
        co_await send_mutex_.lock();
        sync::lock_guard send_guard(send_mutex_);
        
        co_return co_await write_frame(stream_, header, payload);
    }
    
    /// Send a ping and wait for pong
    coro::task<bool> ping(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        if (!is_connected()) {
            co_return false;
        }
        
        uint32_t ping_id = id_generator_.next();
        auto pending = std::make_shared<pending_request>();
        
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_[ping_id] = pending;
        }
        
        // Send ping
        auto header = build_ping(ping_id);
        {
            co_await send_mutex_.lock();
            sync::lock_guard send_guard(send_mutex_);
            
            buffer_writer empty;
            bool sent = co_await write_frame(stream_, header, empty);
            if (!sent) {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                pending_requests_.erase(ping_id);
                co_return false;
            }
        }
        
        // Setup timeout
        auto self = this->shared_from_this();
        auto timeout_task = [](std::chrono::milliseconds ms,
                               std::shared_ptr<pending_request> pending)
            -> coro::task<void>
        {
            co_await time::sleep_for(ms);
            if (pending->try_complete()) {
                pending->timed_out = true;
                pending->completion_event.set();
            }
        };
        
        auto* sched = runtime::scheduler::current();
        if (sched) {
            auto task = timeout_task(timeout, pending);
            sched->spawn(task.release());
        }
        
        // Wait for pong
        co_await pending->completion_event.wait();
        
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_.erase(ping_id);
        }
        
        co_return !pending->timed_out;
    }
    
    /// Get the underlying stream (for advanced usage)
    Stream& stream() noexcept { return stream_; }
    const Stream& stream() const noexcept { return stream_; }
    
private:
    explicit rpc_client(Stream stream)
        : stream_(std::move(stream)) {}
    
    /// Start the background receive loop
    void start_receive_loop() {
        auto self = this->shared_from_this();
        auto* sched = runtime::scheduler::current();
        if (sched) {
            auto task = receive_loop(self);
            sched->spawn(task.release());
        }
    }
    
    /// Background task that receives and dispatches responses
    static coro::task<void> receive_loop(ptr self) {
        ELIO_LOG_DEBUG("RPC client receive loop started");
        
        while (self->is_connected()) {
            auto frame = co_await read_frame(self->stream_);
            if (!frame) {
                ELIO_LOG_DEBUG("RPC client receive loop: connection closed");
                self->close();
                break;
            }
            
            auto& [header, payload] = *frame;
            
            // Handle different message types
            switch (header.type) {
                case message_type::response:
                case message_type::error:
                    self->handle_response(header, std::move(payload));
                    break;
                    
                case message_type::pong:
                    self->handle_pong(header.request_id);
                    break;
                    
                case message_type::ping: {
                    // Respond with pong
                    auto pong = build_pong(header.request_id);
                    buffer_writer empty;
                    co_await self->send_mutex_.lock();
                    sync::lock_guard send_guard(self->send_mutex_);
                    co_await write_frame(self->stream_, pong, empty);
                    break;
                }
                    
                default:
                    ELIO_LOG_WARNING("RPC client: unexpected message type {}",
                                    static_cast<int>(header.type));
                    break;
            }
        }
        
        ELIO_LOG_DEBUG("RPC client receive loop ended");
    }
    
    /// Handle a response message
    void handle_response(const frame_header& header, message_buffer payload) {
        std::shared_ptr<pending_request> pending;
        
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_requests_.find(header.request_id);
            if (it == pending_requests_.end()) {
                ELIO_LOG_WARNING("RPC client: received response for unknown request {}",
                                header.request_id);
                return;
            }
            pending = it->second;
        }
        
        // Use atomic try_complete to ensure only one thread sets the response
        if (pending->try_complete()) {
            pending->response_header = header;
            pending->response_data = std::move(payload);
            pending->completion_event.set();
        }
    }
    
    /// Handle a pong message
    void handle_pong(uint32_t request_id) {
        std::shared_ptr<pending_request> pending;
        
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_requests_.find(request_id);
            if (it == pending_requests_.end()) {
                return;
            }
            pending = it->second;
        }
        
        if (pending->try_complete()) {
            pending->completion_event.set();
        }
    }
    
    Stream stream_;
    std::atomic<bool> closed_{false};
    request_id_generator id_generator_;
    
    // Pending requests map
    std::mutex pending_mutex_;
    std::unordered_map<uint32_t, std::shared_ptr<pending_request>> pending_requests_;
    
    // Send mutex for serializing writes
    sync::mutex send_mutex_;
};

/// Type alias for TCP RPC client
using tcp_rpc_client = rpc_client<net::tcp_stream>;

/// Type alias for UDS RPC client
using uds_rpc_client = rpc_client<net::uds_stream>;

} // namespace elio::rpc
