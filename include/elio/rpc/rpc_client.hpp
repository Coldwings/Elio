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
/// auto client = co_await rpc_client<tcp_stream>::connect(addr);
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
#include <elio/net/resolve.hpp>

#include <memory>
#include <array>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <tuple>
#include <type_traits>
#include <string_view>

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

    static constexpr size_t pending_shard_count = 16;
    
    /// Create a new RPC client from an existing stream
    static ptr create(Stream stream) {
        return ptr(new rpc_client(std::move(stream)));
    }
    
    /// Connect to a TCP server and create client
    template<typename... Args>
    static coro::task<std::optional<ptr>> connect(Args&&... args)
    requires std::is_same_v<Stream, net::tcp_stream>
    {
        if constexpr (requires { net::tcp_connect(std::forward<Args>(args)...); }) {
            auto stream = co_await net::tcp_connect(std::forward<Args>(args)...);
            if (!stream) {
                co_return std::nullopt;
            }
            auto client = create(std::move(*stream));
            client->start_receive_loop();
            co_return client;
        } else if constexpr (
            sizeof...(Args) == 2 &&
            std::is_convertible_v<std::tuple_element_t<0, std::tuple<std::decay_t<Args>...>>, std::string_view> &&
            std::is_integral_v<std::tuple_element_t<1, std::tuple<std::decay_t<Args>...>>>) {
            auto forwarded = std::forward_as_tuple(std::forward<Args>(args)...);
            std::string_view host = std::get<0>(forwarded);
            uint16_t port = static_cast<uint16_t>(std::get<1>(forwarded));

            auto addresses = co_await net::resolve_all(host, port);
            for (const auto& addr : addresses) {
                auto stream = co_await net::tcp_connect(addr);
                if (stream) {
                    auto client = create(std::move(*stream));
                    client->start_receive_loop();
                    co_return client;
                }
            }
            co_return std::nullopt;
        } else {
            static_assert(sizeof...(Args) == 0,
                          "rpc_client<tcp_stream>::connect arguments are not supported");
        }
    }

    /// Connect to a TCP server and create client with explicit resolve options
    static coro::task<std::optional<ptr>> connect(std::string_view host,
                                                  uint16_t port,
                                                  net::resolve_options resolve_opts)
    requires std::is_same_v<Stream, net::tcp_stream>
    {
        auto addresses = co_await net::resolve_all(host, port, resolve_opts);
        for (const auto& addr : addresses) {
            auto stream = co_await net::tcp_connect(addr);
            if (stream) {
                auto client = create(std::move(*stream));
                client->start_receive_loop();
                co_return client;
            }
        }
        co_return std::nullopt;
    }
    
    /// Connect to a UDS server and create client
    template<typename... Args>
    static coro::task<std::optional<ptr>> connect(Args&&... args)
    requires std::is_same_v<Stream, net::uds_stream>
    {
        auto stream = co_await net::uds_connect(std::forward<Args>(args)...);
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
        for (auto& shard : pending_shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            for (auto& [id, req] : shard.requests) {
                if (req->try_complete()) {
                    req->error = rpc_error::connection_closed;
                    req->completion_event.set();
                }
            }
            shard.requests.clear();
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
            auto& shard = pending_shard_for(request_id);
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.requests[request_id] = pending;
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
        auto request_frame = build_request(request_id, Method::id, request, timeout_ms);
        
        {
            co_await send_mutex_.lock();
            sync::lock_guard send_guard(send_mutex_);
            
            bool sent = co_await write_frame(stream_, request_frame.first, request_frame.second);
            if (!sent) {
                auto& shard = pending_shard_for(request_id);
                std::lock_guard<std::mutex> lock(shard.mutex);
                shard.requests.erase(request_id);
                co_return rpc_result<Response>(rpc_error::connection_closed);
            }
        }
        
        // Wait for response with timeout
        // Spawn timeout watcher
        auto* sched = runtime::scheduler::current();
        if (sched) {
            sched->go([ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout),
                       p = pending, tok = token]() mutable {
                return [](std::chrono::milliseconds ms,
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
                }(ms, p, std::move(tok));
            });
        }
        
        // Wait for completion (either response, timeout, or cancellation)
        co_await pending->completion_event.wait();
        
        // Unregister cancellation callback
        cancel_registration.unregister();
        
        // Remove from pending
        {
            auto& shard = pending_shard_for(request_id);
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.requests.erase(request_id);
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
        auto request_frame = build_request(request_id, Method::id, request);
        
        co_await send_mutex_.lock();
        sync::lock_guard send_guard(send_mutex_);
        
        co_return co_await write_frame(stream_, request_frame.first, request_frame.second);
    }
    
    /// Send a ping and wait for pong
    coro::task<bool> ping(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        if (!is_connected()) {
            co_return false;
        }
        
        uint32_t ping_id = id_generator_.next();
        auto pending = std::make_shared<pending_request>();
        
        {
            auto& shard = pending_shard_for(ping_id);
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.requests[ping_id] = pending;
        }
        
        // Send ping
        auto header = build_ping(ping_id);
        {
            co_await send_mutex_.lock();
            sync::lock_guard send_guard(send_mutex_);
            
            buffer_writer empty;
            bool sent = co_await write_frame(stream_, header, empty);
            if (!sent) {
                auto& shard = pending_shard_for(ping_id);
                std::lock_guard<std::mutex> lock(shard.mutex);
                shard.requests.erase(ping_id);
                co_return false;
            }
        }
        
        // Setup timeout
        auto* sched = runtime::scheduler::current();
        if (sched) {
            sched->go([ms = timeout, p = pending]() { 
                return [](std::chrono::milliseconds ms, std::shared_ptr<pending_request> p)
                    -> coro::task<void> {
                    co_await time::sleep_for(ms);
                    if (p->try_complete()) {
                        p->timed_out = true;
                        p->completion_event.set();
                    }
                }(ms, p);
            });
        }
        
        // Wait for pong
        co_await pending->completion_event.wait();
        
        {
            auto& shard = pending_shard_for(ping_id);
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.requests.erase(ping_id);
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
            sched->go([s = self]() { return receive_loop(s); });
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
            auto& shard = pending_shard_for(header.request_id);
            std::lock_guard<std::mutex> lock(shard.mutex);
            auto it = shard.requests.find(header.request_id);
            if (it == shard.requests.end()) {
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
            auto& shard = pending_shard_for(request_id);
            std::lock_guard<std::mutex> lock(shard.mutex);
            auto it = shard.requests.find(request_id);
            if (it == shard.requests.end()) {
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

    struct pending_shard {
        std::mutex mutex;
        std::unordered_map<uint32_t, std::shared_ptr<pending_request>> requests;
    };

    pending_shard& pending_shard_for(uint32_t request_id) noexcept {
        return pending_shards_[request_id % pending_shard_count];
    }

    std::array<pending_shard, pending_shard_count> pending_shards_;
    
    // Send mutex for serializing writes
    sync::mutex send_mutex_;
};

/// Type alias for TCP RPC client
using tcp_rpc_client = rpc_client<net::tcp_stream>;

/// Type alias for UDS RPC client
using uds_rpc_client = rpc_client<net::uds_stream>;

} // namespace elio::rpc
