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
#include <elio/coro/cancel_token.hpp>
#include <elio/net/tcp.hpp>
#include <elio/net/uds.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/runtime/worker_thread.hpp>
#include <elio/time/timer.hpp>
#include <elio/log/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sys/socket.h>
#include <unordered_map>

namespace elio::rpc {

// ============================================================================
// Server configuration
// ============================================================================

/// Configuration for an RPC server. All limits are enforced per-connection
/// except where noted. The defaults are tuned to mitigate the "slow loris"
/// class of attacks (peer trickling bytes to consume sockets indefinitely)
/// without affecting well-behaved clients.
struct rpc_server_config {
    /// Maximum number of concurrent client sessions accepted by serve().
    /// New connections beyond this limit are accepted-then-closed (so the
    /// kernel doesn't keep the SYN backlog full) until existing sessions
    /// finish. 0 means "unlimited" (legacy behaviour, NOT recommended).
    size_t max_sessions = 1024;

    /// Per-frame read deadline. If a peer takes longer than this to send a
    /// complete frame (header + payload + optional checksum), the session
    /// is torn down. 0s disables the deadline (NOT recommended).
    std::chrono::seconds frame_read_timeout{30};

    /// Maximum payload bytes (header excluded) accepted per frame. Defaults
    /// to the protocol-wide max (16 MiB). Set to a smaller value when you
    /// know your application's largest expected message — reducing this is
    /// the most effective defence against memory-exhaustion DoS.
    uint32_t max_message_size = elio::rpc::max_message_size;
};

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

/// A single client session on the server.
///
/// Concurrency model: the read loop streams in one frame at a time and
/// dispatches each request via `runtime::scheduler::current()->go(...)` so
/// out-of-order completion is possible (matching what the client promises).
/// Writes are serialised by `send_mutex_` so concurrent handlers cannot
/// interleave bytes on the wire.
///
/// Lifetime: the session keeps itself alive via shared_from_this() while
/// any dispatched handler is in flight; the run loop's enable_shared_from_this
/// pin is dropped only after every handler has completed.
template<typename Stream>
class rpc_session : public std::enable_shared_from_this<rpc_session<Stream>> {
public:
    using ptr = std::shared_ptr<rpc_session>;
    using handler_map = std::unordered_map<method_id_t, raw_handler_t>;

    /// Create a new session. The handler map is snapshot by value so a
    /// later call to register_method on the server cannot race with the
    /// session's reads.
    static ptr create(Stream stream,
                      std::shared_ptr<const handler_map> handlers,
                      rpc_server_config config = {})
    {
        return ptr(new rpc_session(std::move(stream), std::move(handlers), config));
    }

    /// Run the session (process requests until disconnect).
    coro::task<void> run() {
        auto self = this->shared_from_this();
        ELIO_LOG_DEBUG("RPC session started");

        while (stream_.is_valid() && !closed_.load(std::memory_order_acquire)) {
            auto frame = co_await read_frame_with_deadline();
            if (!frame) {
                ELIO_LOG_DEBUG("RPC session: connection closed");
                break;
            }

            auto& [header, payload] = *frame;

            switch (header.type) {
                case message_type::request:
                    dispatch_request(header, std::move(payload));
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

    /// Close the session. Forces the read loop out of any pending recv.
    void close() {
        if (!closed_.exchange(true, std::memory_order_acq_rel)) {
            // Force any pending recv to return so the run loop wakes up.
            // Going through the stream's ``shutdown_socket()`` (instead of
            // ::shutdown on a raw fd) lets a tls_stream record that its
            // socket is dead so its destructor can skip the close_notify
            // write that risks SIGPIPE on OpenSSL builds without
            // MSG_NOSIGNAL.
            stream_.shutdown_socket();
        }
    }

    /// Check if session is closed
    bool is_closed() const noexcept {
        return closed_.load(std::memory_order_acquire);
    }

private:
    rpc_session(Stream stream,
                std::shared_ptr<const handler_map> handlers,
                rpc_server_config config)
        : stream_(std::move(stream))
        , handlers_(std::move(handlers))
        , config_(config) {}

    /// Read a frame, enforcing config_.frame_read_timeout. The watchdog
    /// shutdown(2)s the socket so any pending recv returns EOF/error.
    ///
    /// The watchdog blocks in a single cancellable sleep_for(timeout, token);
    /// when the frame arrives early the main path cancels the source and the
    /// watchdog returns immediately. cancellable_sleep_awaitable owns its
    /// cancel state via shared_ptr (PR #81), so this is race-free.
    ///
    /// Worker affinity: the watchdog is pinned to the same worker as the
    /// read loop via go_joinable_to(). The read loop itself is pinned by
    /// I/O affinity (each recv re-binds the vthread to the worker whose
    /// io_context owns the completion). Without this pin, autoscaler shrink
    /// can stop the worker hosting the watchdog while its sleep timer is
    /// still pending — the io_context is no longer polled, the watchdog
    /// never resumes, and the read loop hangs forever on `co_await *watchdog`.
    /// scheduler::set_thread_count's I/O drain has a 5 s bound while
    /// frame_read_timeout defaults to 30 s, so the leak is reachable in
    /// practice. Pinning makes shrink-time draining a shared-fate operation:
    /// either both the read loop and watchdog drain, or neither does (and
    /// the connection is correctly torn down at scheduler shutdown).
    ///
    /// Frame-vs-watchdog race: if the timer CQE fires microseconds before
    /// read_frame_bounded returns a complete frame, both `frame` and
    /// `timed_out` end up set. The contract is "must arrive by deadline" —
    /// the frame did, so we deliver it. ::shutdown() has already happened
    /// inside the watchdog, so the next read_frame_with_deadline call will
    /// fail naturally; that's the right behavior for one-shot delivery.
    coro::task<std::optional<std::pair<frame_header, message_buffer>>>
    read_frame_with_deadline() {
        if (config_.frame_read_timeout.count() <= 0) {
            // No deadline configured.
            co_return co_await read_frame_bounded(stream_, config_.max_message_size);
        }

        auto* sched = runtime::scheduler::current();
        if (!sched) {
            // Cannot spawn a watchdog without a scheduler; degrade gracefully.
            co_return co_await read_frame_bounded(stream_, config_.max_message_size);
        }

        auto timed_out = std::make_shared<std::atomic<bool>>(false);
        auto* stream_ptr = &stream_;
        auto timeout = config_.frame_read_timeout;
        coro::cancel_source watchdog_cancel;
        auto wd_token = watchdog_cancel.get_token();

        // Pin the watchdog to the same worker as the read loop so it cannot
        // be independently shrunk by the autoscaler — otherwise a doomed
        // worker can have an in-flight timeout SQE pending while its
        // io_context stops being polled, leaving us hung on
        // ``co_await *watchdog``.
        auto* current_worker = runtime::worker_thread::current();
        auto watchdog_body = [stream_ptr, timed_out, timeout, wd_token]()
                                 -> coro::task<void> {
            auto result = co_await elio::time::sleep_for(timeout, wd_token);
            if (result == coro::cancel_result::completed) {
                // Sleep ran to completion without being cancelled —
                // the frame did not arrive in time. Going through the
                // stream's ``shutdown_socket()`` (rather than raw
                // ``::shutdown(fd, ...)``) lets a tls_stream record that
                // its socket is dead so its destructor can skip the
                // close_notify write that risks SIGPIPE on OpenSSL
                // builds without MSG_NOSIGNAL.
                timed_out->store(true, std::memory_order_release);
                stream_ptr->shutdown_socket();
            }
            co_return;
        };
        auto watchdog = current_worker
            ? sched->go_joinable_to(current_worker->worker_id(), watchdog_body)
            : sched->go_joinable(watchdog_body);

        auto frame = co_await read_frame_bounded(stream_, config_.max_message_size);

        // Wake the watchdog so it returns immediately instead of sleeping
        // out the rest of the deadline. Move the join_handle into the
        // await: ``co_await watchdog`` (lvalue) can make some compilers
        // instantiate join_handle's deleted copy ctor while
        // materializing the awaitable. The owning rvalue form is the
        // portable, intent-clear one.
        watchdog_cancel.cancel();
        co_await std::move(watchdog);

        // Prefer the arrived frame even when the watchdog raced ahead and
        // already set timed_out. The contract is delivery-by-deadline; the
        // frame met it. If the watchdog also issued ::shutdown() the socket
        // is dead — but this frame is still complete and worth processing.
        if (frame) {
            co_return frame;
        }
        if (timed_out->load(std::memory_order_acquire)) {
            ELIO_LOG_WARNING("RPC session: frame read timed out after {}s",
                             timeout.count());
        }
        co_return std::nullopt;
    }

    /// Dispatch a request handler to the scheduler so the read loop can
    /// continue receiving the next frame (out-of-order completion).
    void dispatch_request(frame_header header, message_buffer payload) {
        auto* sched = runtime::scheduler::current();
        if (!sched) {
            ELIO_LOG_ERROR("RPC session: no current scheduler; dropping request {}",
                           header.request_id);
            return;
        }
        auto self = this->shared_from_this();
        sched->go([self, hdr = header, pl = std::move(payload)]() mutable
                      -> coro::task<void> {
            co_await self->handle_request(hdr, std::move(pl));
        });
    }

    /// Handle an incoming request (runs concurrently with the read loop).
    coro::task<void> handle_request(const frame_header& header, message_buffer payload) {
        // Find handler
        auto it = handlers_->find(header.method_id);
        if (it == handlers_->end()) {
            ELIO_LOG_WARNING("RPC session: method {} not found", header.method_id);
            auto error_frame = build_error_response(
                header.request_id,
                rpc_error::method_not_found,
                "Method not found"
            );
            co_await send_response(error_frame.first, error_frame.second);
            co_return;
        }

        // Build context
        rpc_context ctx;
        ctx.request_id = header.request_id;
        ctx.method_id = header.method_id;

        // Call handler and capture result or error
        bool handler_success = false;
        buffer_writer response_payload;
        cleanup_callback_t cleanup_cb;
        rpc_error error_code = rpc_error::success;
        std::string error_message;

        try {
            // Parse timeout flag inside the try block: a malformed
            // has_timeout flag with payload_length < 4 must surface as
            // rpc_error::invalid_message instead of tearing the session
            // down (it would otherwise escape this coroutine via an
            // uncaught serialization_error).
            buffer_view view = payload.view();
            bool malformed_timeout = false;
            if (has_flag(header.flags, message_flags::has_timeout)) {
                if (view.remaining() < sizeof(uint32_t)) {
                    error_code = rpc_error::invalid_message;
                    error_message = "has_timeout flag set but payload too short";
                    malformed_timeout = true;
                } else {
                    ctx.timeout_ms = view.read<uint32_t>();
                }
            }

            if (!malformed_timeout) {
                auto result = co_await it->second(ctx, view);
                handler_success = result.success;
                response_payload = std::move(result.payload);
                cleanup_cb = std::move(result.cleanup);
                if (!handler_success) {
                    error_code = rpc_error::internal_error;
                    error_message = "Handler failed";
                }
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

        // Send response (outside exception handlers).
        // Track whether the write succeeded — cleanup callbacks are only
        // invoked after a *successful* send per the documented contract.
        bool sent = false;
        try {
            if (handler_success) {
                frame_header resp_header;
                resp_header.request_id = header.request_id;
                resp_header.type = message_type::response;
                resp_header.flags = message_flags::none;
                resp_header.method_id = 0;
                validate_payload_size(response_payload.size());
                resp_header.payload_length = static_cast<uint32_t>(response_payload.size());

                sent = co_await send_response(resp_header, response_payload);
            } else {
                auto error_frame = build_error_response(
                    header.request_id,
                    error_code,
                    error_message
                );
                sent = co_await send_response(error_frame.first, error_frame.second);
            }
        } catch (const std::exception& e) {
            ELIO_LOG_ERROR("RPC session: send_response exception: {}", e.what());
        }

        // Invoke cleanup callback only when the response was successfully
        // sent. If send_response threw, we still invoke cleanup here (via
        // the try/catch above the send block) because the handler already
        // completed and resources need releasing regardless. However, if
        // the write simply returned false (peer disconnected), skip cleanup
        // per the documented contract.
        if (cleanup_cb && sent) {
            try {
                cleanup_cb();
            } catch (const std::exception& e) {
                ELIO_LOG_ERROR("RPC session: cleanup callback exception: {}", e.what());
            }
        }
    }

    /// Send a response frame; serialised against other concurrent handlers
    /// on the same connection by `send_mutex_`.
    /// @return true if the frame was written successfully, false otherwise.
    coro::task<bool> send_response(const frame_header& header,
                                    const buffer_writer& payload)
    {
        co_await send_mutex_.lock();
        sync::lock_guard guard(send_mutex_);
        co_return co_await write_frame(stream_, header, payload);
    }

    Stream stream_;
    /// Snapshot of the server's handler map captured at session creation.
    /// Guarantees the read loop never observes a torn map (Fix 5).
    std::shared_ptr<const handler_map> handlers_;
    rpc_server_config config_;
    std::atomic<bool> closed_{false};
    sync::mutex send_mutex_;
};

// ============================================================================
// RPC Server
// ============================================================================

/// RPC server that accepts connections and handles requests.
///
/// Lifecycle:
///   1. Construct with optional rpc_server_config.
///   2. Call register_method<...>() any number of times BEFORE serve().
///   3. Call serve() (or handle_client()) to begin accepting connections.
///      At this moment the handler map is "frozen": further calls to
///      register_method() will throw std::logic_error.
///
/// This freeze model (Fix 5) guarantees that a session's `handlers_` view
/// is immutable for its entire lifetime, eliminating the data race that
/// existed when sessions held a `const unordered_map&` to a map the server
/// kept mutating.
template<typename Stream>
class rpc_server {
public:
    using stream_type = Stream;
    using session_type = rpc_session<Stream>;
    using session_ptr = typename session_type::ptr;
    using handler_map = typename session_type::handler_map;

    rpc_server() = default;
    explicit rpc_server(rpc_server_config config) : config_(config) {}
    ~rpc_server() = default;

    // Non-copyable, non-movable
    rpc_server(const rpc_server&) = delete;
    rpc_server& operator=(const rpc_server&) = delete;
    rpc_server(rpc_server&&) = delete;
    rpc_server& operator=(rpc_server&&) = delete;

    /// Read-only access to the configuration.
    const rpc_server_config& config() const noexcept { return config_; }
    
    /// Register a method handler
    /// @tparam Method The method descriptor type
    /// @tparam Handler A callable returning task<Response>
    /// @throws std::invalid_argument if Method::id was already registered
    /// @throws std::logic_error if called after serve() has started
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

        insert_handler(Method::id, std::move(raw_handler));
    }

    /// Register a method handler with context
    /// @tparam Method The method descriptor type
    /// @tparam Handler A callable taking (ctx, request) and returning task<Response>
    /// @throws std::invalid_argument if Method::id was already registered
    /// @throws std::logic_error if called after serve() has started
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

        insert_handler(Method::id, std::move(raw_handler));
    }

    /// Register a synchronous method handler (doesn't need co_await)
    /// @throws std::invalid_argument if Method::id was already registered
    /// @throws std::logic_error if called after serve() has started
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

        insert_handler(Method::id, std::move(raw_handler));
    }
    
    /// Register a method handler with cleanup callback support
    /// Handler should return std::pair<Response, cleanup_callback_t>
    /// The cleanup callback is invoked after the response is successfully sent
    /// @tparam Method The method descriptor type
    /// @tparam Handler A callable returning task<std::pair<Response, cleanup_callback_t>>
    /// @throws std::invalid_argument if Method::id was already registered
    /// @throws std::logic_error if called after serve() has started
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

        insert_handler(Method::id, std::move(raw_handler));
    }

    /// Register a method handler with context and cleanup callback support
    /// Handler should return std::pair<Response, cleanup_callback_t>
    /// @tparam Method The method descriptor type
    /// @tparam Handler A callable taking (ctx, request) and returning task<std::pair<Response, cleanup_callback_t>>
    /// @throws std::invalid_argument if Method::id was already registered
    /// @throws std::logic_error if called after serve() has started
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

        insert_handler(Method::id, std::move(raw_handler));
    }
    
    /// Serve connections from a TCP listener
    coro::task<void> serve(net::tcp_listener& listener)
    requires std::is_same_v<Stream, net::tcp_stream>
    {
        freeze_handlers();
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

            // Backpressure: reject new sessions when at the configured cap.
            // We accept then immediately drop so the kernel's listen backlog
            // doesn't fill up.
            if (!try_reserve_session_slot()) {
                ELIO_LOG_WARNING(
                    "RPC server: max_sessions={} reached, rejecting new connection",
                    config_.max_sessions);
                continue;  // stream dtor closes the socket
            }

            // Create and start session
            auto session = session_type::create(std::move(*stream),
                                                 frozen_handlers_, config_);

            // Track session
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                sessions_.push_back(session);
            }

            // Spawn session handler
            auto* sched = runtime::scheduler::current();
            if (sched) {
                sched->go([this, s = session]() { return run_session(s); });
            } else {
                // No scheduler available - remove session from tracking and release slot
                remove_session(session);
                release_session_slot();
            }
        }

        ELIO_LOG_INFO("RPC server stopped");
    }

    /// Serve connections from a UDS listener
    coro::task<void> serve(net::uds_listener& listener)
    requires std::is_same_v<Stream, net::uds_stream>
    {
        freeze_handlers();
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

            if (!try_reserve_session_slot()) {
                ELIO_LOG_WARNING(
                    "RPC server: max_sessions={} reached, rejecting new connection",
                    config_.max_sessions);
                continue;
            }

            // Create and start session
            auto session = session_type::create(std::move(*stream),
                                                 frozen_handlers_, config_);

            // Track session
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                sessions_.push_back(session);
            }

            // Spawn session handler
            auto* sched = runtime::scheduler::current();
            if (sched) {
                sched->go([this, s = session]() { return run_session(s); });
            } else {
                // No scheduler available - remove session from tracking and release slot
                remove_session(session);
                release_session_slot();
            }
        }

        ELIO_LOG_INFO("RPC server stopped");
    }

    /// Handle a single client stream (useful for testing or custom accept logic)
    coro::task<void> handle_client(Stream stream) {
        freeze_handlers();
        if (!try_reserve_session_slot()) {
            ELIO_LOG_WARNING(
                "RPC server: max_sessions={} reached, dropping client",
                config_.max_sessions);
            co_return;  // stream dtor closes the socket
        }
        auto session = session_type::create(std::move(stream),
                                             frozen_handlers_, config_);

        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions_.push_back(session);
        }

        co_await session->run();

        // Remove from active sessions (swap-and-pop for O(1) removal)
        remove_session(session);
        release_session_slot();
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
    /// Insert a handler entry, rejecting duplicates and post-serve writes.
    /// (Fixes 5 and 8.)
    void insert_handler(method_id_t id, raw_handler_t handler) {
        if (handlers_frozen_.load(std::memory_order_acquire)) {
            throw std::logic_error(
                "rpc_server: register_method called after serve() has started");
        }
        auto [it, inserted] = handlers_.emplace(id, std::move(handler));
        (void)it;
        if (!inserted) {
            throw std::invalid_argument(
                "rpc_server: duplicate method id");
        }
    }

    /// Snapshot the handler map so sessions hold an immutable view.
    /// Idempotent: subsequent calls are no-ops. Thread-safe: concurrent
    /// callers (e.g. parallel handle_client() entries) all see the same
    /// snapshot once published.
    void freeze_handlers() {
        std::call_once(freeze_once_, [this]() {
            frozen_handlers_ = std::make_shared<const handler_map>(handlers_);
            handlers_frozen_.store(true, std::memory_order_release);
        });
    }

    /// Reserve a session slot if max_sessions has not been reached.
    /// max_sessions == 0 means unlimited.
    bool try_reserve_session_slot() noexcept {
        if (config_.max_sessions == 0) {
            active_sessions_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        size_t cur = active_sessions_.load(std::memory_order_relaxed);
        while (cur < config_.max_sessions) {
            if (active_sessions_.compare_exchange_weak(
                    cur, cur + 1, std::memory_order_acq_rel)) {
                return true;
            }
        }
        return false;
    }

    void release_session_slot() noexcept {
        active_sessions_.fetch_sub(1, std::memory_order_acq_rel);
    }

    /// Remove a session from the tracking vector using swap-and-pop (O(1)).
    /// Must be called without sessions_mutex_ held.
    void remove_session(const session_ptr& session) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = std::find(sessions_.begin(), sessions_.end(), session);
        if (it != sessions_.end()) {
            *it = std::move(sessions_.back());
            sessions_.pop_back();
        }
    }

    /// Run a session and clean up when done
    coro::task<void> run_session(session_ptr session) {
        co_await session->run();

        // Remove from active sessions (swap-and-pop for O(1) removal)
        remove_session(session);
        release_session_slot();
    }

    rpc_server_config config_{};
    handler_map handlers_;
    std::atomic<bool> handlers_frozen_{false};
    std::once_flag freeze_once_;
    std::shared_ptr<const handler_map> frozen_handlers_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> active_sessions_{0};

    mutable std::mutex sessions_mutex_;
    std::vector<session_ptr> sessions_;
};

/// Type alias for TCP RPC server
using tcp_rpc_server = rpc_server<net::tcp_stream>;

/// Type alias for UDS RPC server
using uds_rpc_server = rpc_server<net::uds_stream>;

} // namespace elio::rpc
