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
#include <limits>
#include <optional>
#include <tuple>
#include <type_traits>
#include <string_view>
#include <utility>

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

namespace detail {

/// Reserve a request ID by trying generated IDs until one can be inserted.
/// The try_reserve callback must perform the contains+insert step atomically
/// for the relevant pending-request shard.
template<typename NextIdFn, typename TryReserveFn>
inline std::optional<uint32_t> reserve_unique_request_id(
    NextIdFn&& next_id,
    TryReserveFn&& try_reserve,
    size_t max_attempts = static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
    for (size_t attempt = 0; attempt < max_attempts; ++attempt) {
        uint32_t candidate = next_id();
        if (try_reserve(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

} // namespace detail

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
    
    /// Close the client connection.
    /// @note This must be called explicitly to shut down the socket.
    /// Dropping the last shared_ptr without calling close() will NOT
    /// shut down the socket because the receive loop holds a strong
    /// reference across the blocked read, preventing the destructor
    /// from running until the peer sends data or TCP keepalive fires.
    void close() {
        if (closed_.exchange(true, std::memory_order_acq_rel)) {
            return;  // Already closed
        }

        // Shut down the underlying socket to force the receive loop's
        // blocked read to return.  Without this, the receive loop stays
        // alive until the peer sends data or TCP keepalive fires (hours).
        stream_.shutdown_socket();

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
    /// RAII helper that erases a pending request from its shard map on
    /// destruction. This guarantees the entry is removed even if the
    /// caller's coroutine is cancelled / destroyed mid-await — without it,
    /// the shard kept a dangling shared_ptr forever (Fix 4).
    struct pending_eraser {
        rpc_client* self = nullptr;
        uint32_t request_id = 0;
        bool active = false;

        pending_eraser() = default;
        pending_eraser(rpc_client* s, uint32_t id) : self(s), request_id(id), active(true) {}
        pending_eraser(const pending_eraser&) = delete;
        pending_eraser& operator=(const pending_eraser&) = delete;
        pending_eraser(pending_eraser&& other) noexcept
            : self(other.self), request_id(other.request_id), active(other.active) {
            other.active = false;
        }
        pending_eraser& operator=(pending_eraser&& other) noexcept {
            if (this != &other) {
                erase_now();
                self = other.self;
                request_id = other.request_id;
                active = other.active;
                other.active = false;
            }
            return *this;
        }
        ~pending_eraser() { erase_now(); }

        void erase_now() noexcept {
            if (!active || !self) return;
            active = false;
            try {
                auto& shard = self->pending_shard_for(request_id);
                std::lock_guard<std::mutex> lock(shard.mutex);
                shard.requests.erase(request_id);
            } catch (...) {
                // Erase from a noexcept context must not propagate.
            }
        }
    };

    /// Internal implementation of call with cancellation support
    template<typename Method, typename Rep, typename Period>
    coro::task<rpc_result<typename Method::response_type>> call_impl(
        const typename Method::request_type& request,
        std::chrono::duration<Rep, Period> timeout,
        coro::cancel_token token)
    {
        using Response = typename Method::response_type;

        // Keep the client alive for the full duration of this coroutine.
        // The frame stores raw `this` (and `pending_eraser` stores a raw
        // rpc_client*) across every co_await below. Without this strong
        // self-reference, dropping the last external shared_ptr while the
        // call is suspended (waiting for send, response, timeout, or the
        // completion_event) would run ~rpc_client() and free members such
        // as stream_, send_mutex_ and pending_shards_ that the coroutine
        // still touches on resumption — a use-after-free.
        auto self = this->shared_from_this();

        // Check if already cancelled
        if (token.is_cancelled()) {
            co_return rpc_result<Response>(rpc_error::cancelled);
        }

        if (!is_connected()) {
            co_return rpc_result<Response>(rpc_error::connection_closed);
        }

        // Reserve a request ID before sending. The wire ID is uint32_t and
        // wraps, so skip IDs that are still in-flight instead of overwriting
        // the pending entry for an older call.
        auto reserved = reserve_pending_request();
        if (!reserved) {
            co_return rpc_result<Response>(rpc_error::internal_error);
        }
        uint32_t request_id = reserved->request_id;
        auto pending = std::move(reserved->pending);

        // From here on, the shard entry is guaranteed to be cleaned up no
        // matter how this coroutine exits (normal return, exception, or
        // caller-driven destruction while awaiting completion_event).
        pending_eraser eraser(this, request_id);

        // Register cancellation callback. The callback completes the local
        // pending request; the coroutine starts the wire-level cancel send
        // after it resumes so callbacks never block on socket I/O.
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
        bool request_sent = false;

        {
            co_await send_mutex_.lock();
            sync::lock_guard send_guard(send_mutex_);

            if (pending->is_completed()) {
                co_return rpc_result<Response>(pending->error);
            }

            bool sent = co_await write_frame(stream_, request_frame.first, request_frame.second);
            if (!sent) {
                co_return rpc_result<Response>(rpc_error::connection_closed);
            }
            request_sent = true;
        }

        if (pending->is_completed()) {
            if (pending->error == rpc_error::cancelled && request_sent) {
                if (!start_cancel_frame(request_id)) {
                    co_await send_cancel_frame(request_id);
                }
            }
            co_return rpc_result<Response>(pending->error);
        }

        // Wait for response with timeout
        // Spawn timeout watcher. Use a per-call local cancellation source
        // (rather than the caller-supplied token) so the watcher can be
        // cancelled promptly whenever the request completes for ANY reason —
        // response, error, caller cancellation, or connection close — not
        // only when the caller cancels. Without this the sleeping watcher
        // would linger for the full timeout on every fast/normal completion,
        // retaining the pending_request and an idle background task (mirrors
        // the pattern already used by ping()).
        coro::cancel_source timeout_cancel;
        auto* sched = runtime::scheduler::current();
        if (sched) {
            sched->go([ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout),
                       p = pending, tok = timeout_cancel.get_token()]() mutable {
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

        // Cancel the timeout watcher so it stops sleeping immediately once
        // the request has completed, regardless of how completion happened.
        timeout_cancel.cancel();

        // Unregister cancellation callback
        cancel_registration.unregister();

        if (pending->error == rpc_error::cancelled && request_sent) {
            if (!start_cancel_frame(request_id)) {
                co_await send_cancel_frame(request_id);
            }
        }

        // eraser's destructor removes the shard entry below.

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

    /// Best-effort wire-level cancellation for a request that has already
    /// been sent. The caller has already completed locally with cancelled.
    bool start_cancel_frame(uint32_t request_id) {
        auto* sched = runtime::scheduler::current();
        if (!sched) {
            return false;
        }

        auto self = this->shared_from_this();
        sched->go([self, request_id]() -> coro::task<void> {
            co_await self->send_cancel_frame(request_id);
        });
        return true;
    }

    coro::task<void> send_cancel_frame(uint32_t request_id) {
        if (!is_connected()) {
            co_return;
        }

        auto header = build_cancel(request_id);
        buffer_writer empty;

        co_await send_mutex_.lock();
        sync::lock_guard send_guard(send_mutex_);

        if (!is_connected()) {
            co_return;
        }

        (void)co_await write_frame(stream_, header, empty);
    }
    
public:
    /// Send a one-way message (no response expected)
    template<typename Method>
    coro::task<bool> send_oneway(const typename Method::request_type& request) {
        // Hold a strong self-reference: the write below suspends while it
        // touches send_mutex_ and stream_, so the client must outlive it.
        auto self = this->shared_from_this();

        if (!is_connected()) {
            co_return false;
        }

        // One-way calls do not create a pending entry, but the server may
        // still emit a response/error frame for the request. Avoid reusing a
        // currently pending ID so such a late frame cannot complete another
        // in-flight call or ping.
        auto request_id = next_unoccupied_request_id();
        if (!request_id) {
            co_return false;
        }
        auto request_frame = build_oneway_request(
            *request_id, Method::id, request);

        co_await send_mutex_.lock();
        sync::lock_guard send_guard(send_mutex_);

        co_return co_await write_frame(stream_, request_frame.first, request_frame.second);
    }

    /// Send a ping and wait for pong
    coro::task<bool> ping(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        // Hold a strong self-reference for the coroutine's lifetime. Like
        // call_impl(), this suspends on send_mutex_, stream_ writes and the
        // pending_request completion_event, and pending_eraser stores a raw
        // rpc_client*; dropping the last external shared_ptr mid-ping would
        // otherwise free those members and cause a use-after-free.
        auto self = this->shared_from_this();

        if (!is_connected()) {
            co_return false;
        }

        auto reserved = reserve_pending_request(rpc_error::invalid_message);
        if (!reserved) {
            co_return false;
        }
        uint32_t ping_id = reserved->request_id;
        auto pending = std::move(reserved->pending);

        // Guaranteed cleanup of the shard entry on every exit path.
        pending_eraser eraser(this, ping_id);

        // Send ping
        auto header = build_ping(ping_id);
        {
            co_await send_mutex_.lock();
            sync::lock_guard send_guard(send_mutex_);

            buffer_writer empty;
            bool sent = co_await write_frame(stream_, header, empty);
            if (!sent) {
                co_return false;
            }
        }

        // Setup timeout with cancellation support so the watcher exits
        // promptly when ping completes or is destroyed (mirrors call_impl).
        coro::cancel_source ping_cancel;
        auto* sched = runtime::scheduler::current();
        if (sched) {
            sched->go([ms = timeout, p = pending, tok = ping_cancel.get_token()]() {
                return [](std::chrono::milliseconds ms,
                          std::shared_ptr<pending_request> p,
                          coro::cancel_token tok)
                    -> coro::task<void> {
                    auto result = co_await time::sleep_for(ms, tok);
                    if (result == coro::cancel_result::completed && p->try_complete()) {
                        p->timed_out = true;
                        p->error = rpc_error::timeout;
                        p->completion_event.set();
                    }
                }(ms, p, std::move(tok));
            });
        }

        // Wait for pong
        co_await pending->completion_event.wait();

        // Cancel the timeout watcher so it doesn't linger after completion.
        ping_cancel.cancel();

        co_return pending->error == rpc_error::success && !pending->timed_out;
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
            // Capture a weak_ptr so the receive loop does not keep the
            // client alive when all external owners have dropped their
            // references.  Without this, dropping the shared_ptr returned
            // by connect() would leak the stream fd and coroutine frame
            // forever because the receive loop's strong ref prevents
            // destruction (and therefore close()).
            std::weak_ptr<rpc_client> weak_self = self;
            sched->go([w = std::move(weak_self)]() { return receive_loop(w); });
        }
    }

    /// Background task that receives and dispatches responses
    static coro::task<void> receive_loop(std::weak_ptr<rpc_client> weak_self) {
        ELIO_LOG_DEBUG("RPC client receive loop started");

        while (true) {
            auto self = weak_self.lock();
            if (!self || !self->is_connected()) {
                break;
            }

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
            pending->error = rpc_error::success;
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

    struct reserved_pending_request {
        uint32_t request_id = 0;
        std::shared_ptr<pending_request> pending;
    };

    std::optional<reserved_pending_request> reserve_pending_request(
        rpc_error initial_error = rpc_error::success) {
        auto pending = std::make_shared<pending_request>();
        pending->error = initial_error;
        auto request_id = detail::reserve_unique_request_id(
            [this]() {
                return id_generator_.next();
            },
            [this, &pending](uint32_t candidate) {
                auto& shard = pending_shard_for(candidate);
                std::lock_guard<std::mutex> lock(shard.mutex);
                return shard.requests.emplace(candidate, pending).second;
            });

        if (!request_id) {
            return std::nullopt;
        }
        return reserved_pending_request{*request_id, std::move(pending)};
    }

    std::optional<uint32_t> next_unoccupied_request_id() {
        return detail::reserve_unique_request_id(
            [this]() {
                return id_generator_.next();
            },
            [this](uint32_t candidate) {
                auto& shard = pending_shard_for(candidate);
                std::lock_guard<std::mutex> lock(shard.mutex);
                return shard.requests.find(candidate) == shard.requests.end();
            });
    }

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
