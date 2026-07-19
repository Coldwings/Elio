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
#include <elio/coro/task_group.hpp>
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

/// Policy applied when one session reaches its in-flight request cap.
enum class rpc_request_overload_policy {
    /// Reject the excess request with rpc_error::resource_exhausted when a
    /// response is allowed and no previous overload rejection is still being
    /// written. no_response requests, and further response-capable requests
    /// while an overload rejection is already in flight, are dropped so the
    /// overload path itself cannot grow without bound.
    reject_request,
    /// Close the session immediately.
    close_session,
};

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

    /// Maximum active request slots for one client session. A response-capable
    /// request holds its slot until its handler has produced a result and the
    /// response/error send path has completed or failed; a no_response request
    /// holds its slot until its handler finishes. 0 means "unlimited" (legacy
    /// behaviour). Set this for exposed services so one well-formed client
    /// cannot consume unbounded request lifecycle concurrency.
    size_t max_in_flight_requests_per_session = 0;

    /// Strategy for requests that arrive after the per-session in-flight cap.
    rpc_request_overload_policy request_overload_policy =
        rpc_request_overload_policy::reject_request;
};

// ============================================================================
// RPC Context
// ============================================================================

/// Cleanup callback type - invoked after a normal response is sent, or after
/// serialized no_response one-way payload data can be discarded.
using cleanup_callback_t = std::function<void()>;

/// Context passed to RPC handlers
struct rpc_context {
    uint32_t request_id;                    ///< Request ID for correlation
    method_id_t method_id;                  ///< Method being called
    std::optional<uint32_t> timeout_ms;     ///< Client-specified timeout (if any)
    /// Cancelled by a client cancel frame or session teardown.
    coro::cancel_token cancel_token;
    
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
    cleanup_callback_t cleanup;  ///< Optional cleanup callback for response-owned data
    
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
/// dispatches each request into a session-owned task scope so out-of-order
/// completion is possible (matching what the client promises).
/// `rpc_server_config::max_in_flight_requests_per_session` can bound that
/// per-session active request lifecycle and choose the overload strategy.
/// Writes are serialised by `send_mutex_` so concurrent handlers cannot
/// interleave bytes on the wire.
///
/// Lifetime: run() pins the session while the task scope owns, cancels, and
/// joins every accepted request and control-frame task.
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

    /// Run the session (process requests until disconnect), then cancel and
    /// join every request and control-frame task accepted by this session.
    coro::task<void> run() {
        // session_scope_body stores a raw pointer; this pin outlives its scope.
        [[maybe_unused]] auto lifetime = this->shared_from_this();
        ELIO_LOG_DEBUG("RPC session started");

        coro::task_group_options options;
        options.failure_policy = coro::task_group_failure_policy::collect_all;
        try {
            co_await coro::task_scope(session_scope_body{this}, options);
        } catch (const coro::task_group_error& e) {
            close();
            ELIO_LOG_ERROR(
                "RPC session: {} child task failure(s) during teardown",
                e.failures().size());
        } catch (const std::exception& e) {
            close();
            ELIO_LOG_ERROR("RPC session: unhandled exception: {}", e.what());
        } catch (...) {
            close();
            ELIO_LOG_ERROR("RPC session: unhandled unknown exception");
        }
        close();
        ELIO_LOG_DEBUG("RPC session ended");
    }

    /// Close the session. Forces the read loop out of any pending recv.
    void close() noexcept {
        if (!closed_.exchange(true, std::memory_order_acq_rel)) {
            // Force any pending recv to return so the run loop wakes up. If a
            // frame write already started, cancel its writable-poll wait and
            // defer shutdown until that write leaves its guarded section so
            // shutdown cannot race it.
            // Going through the stream's ``shutdown_socket()`` (instead of
            // ::shutdown on a raw fd) lets a tls_stream record that its socket
            // is dead so its destructor can skip the close_notify write that
            // risks SIGPIPE on OpenSSL builds without MSG_NOSIGNAL.
            cancel_source_noexcept(frame_write_cancel_, "frame write");
            request_stream_shutdown();
            cancel_all_active_requests();
        }
    }

    /// Check if session is closed
    bool is_closed() const noexcept {
        return closed_.load(std::memory_order_acquire);
    }

private:
    struct session_scope_body {
        rpc_session* self;

        coro::task<void> operator()(coro::task_group& group) {
            return self->run_scope(group);
        }
    };

    struct active_request_state {
        coro::cancel_source cancel;
    };

    enum class active_request_reservation {
        reserved,
        duplicate,
        limited,
        closed,
    };

    class frame_write_guard {
    public:
        explicit frame_write_guard(rpc_session* self) noexcept : self_(self) {}
        ~frame_write_guard() {
            if (self_) {
                self_->finish_frame_write();
            }
        }

        frame_write_guard(const frame_write_guard&) = delete;
        frame_write_guard& operator=(const frame_write_guard&) = delete;
        frame_write_guard(frame_write_guard&& other) noexcept
            : self_(other.self_) {
            other.self_ = nullptr;
        }
        frame_write_guard& operator=(frame_write_guard&& other) noexcept {
            if (this != &other) {
                if (self_) {
                    self_->finish_frame_write();
                }
                self_ = other.self_;
                other.self_ = nullptr;
            }
            return *this;
        }

    private:
        rpc_session* self_ = nullptr;
    };

    struct active_request_lease {
        rpc_session* self = nullptr;
        uint32_t request_id = 0;
        std::shared_ptr<active_request_state> active;

        active_request_lease(rpc_session* s,
                             uint32_t id,
                             std::shared_ptr<active_request_state> a)
            : self(s), request_id(id), active(std::move(a)) {}
        active_request_lease(const active_request_lease&) = delete;
        active_request_lease& operator=(const active_request_lease&) = delete;
        ~active_request_lease() {
            if (self && active) {
                self->erase_active_request(request_id, active);
            }
        }
    };

    struct in_flight_flag_lease {
        explicit in_flight_flag_lease(std::atomic<bool>& flag) noexcept
            : flag(std::addressof(flag)) {}
        in_flight_flag_lease(const in_flight_flag_lease&) = delete;
        in_flight_flag_lease& operator=(const in_flight_flag_lease&) = delete;
        ~in_flight_flag_lease() {
            flag->store(false, std::memory_order_release);
        }

        std::atomic<bool>* flag;
    };

    rpc_session(Stream stream,
                std::shared_ptr<const handler_map> handlers,
                rpc_server_config config)
        : stream_(std::move(stream))
        , handlers_(std::move(handlers))
        , config_(config) {}

    coro::task<void> run_scope(coro::task_group& group) {
        [[maybe_unused]] auto close_on_runtime_cancel =
            coro::this_coro::cancel_token().on_cancel([this] {
                close();
            });
        std::exception_ptr read_failure;
        try {
            while (stream_.is_valid() &&
                   !closed_.load(std::memory_order_acquire)) {
                auto frame = co_await read_frame_with_deadline();
                if (!frame) {
                    ELIO_LOG_DEBUG("RPC session: connection closed");
                    break;
                }

                auto& [header, payload] = *frame;
                switch (header.type) {
                    case message_type::request:
                        dispatch_request(group, header, std::move(payload));
                        break;

                    case message_type::ping:
                        schedule_pong(group, header.request_id);
                        break;

                    case message_type::cancel:
                        ELIO_LOG_DEBUG(
                            "RPC session: received cancel for request {}",
                            header.request_id);
                        cancel_request(header.request_id);
                        break;

                    default:
                        ELIO_LOG_WARNING(
                            "RPC session: unexpected message type {}",
                            static_cast<int>(header.type));
                        break;
                }
            }
        } catch (...) {
            read_failure = std::current_exception();
        }

        close();
        cancel_all_active_requests();
        try {
            group.request_cancel();
        } catch (const std::exception& e) {
            ELIO_LOG_ERROR(
                "RPC session: task cancellation callback exception: {}",
                e.what());
        } catch (...) {
            ELIO_LOG_ERROR(
                "RPC session: task cancellation callback unknown exception");
        }

        if (read_failure) {
            std::rethrow_exception(std::move(read_failure));
        }
    }

    /// Read a frame, enforcing config_.frame_read_timeout. The watchdog
    /// shutdown(2)s the socket so any pending recv returns EOF/error.
    ///
    /// The watchdog blocks in a single cancellable sleep_for(timeout, token);
    /// when the frame arrives early the main path cancels the source and the
    /// watchdog returns immediately. cancellable_sleep_awaitable owns its
    /// cancel state via shared_ptr (PR #81), so this is race-free.
    ///
    /// Worker ownership: go_joinable_to() starts the watchdog on the read
    /// loop's current worker. Each pending timer and recv then holds its own
    /// operation-local I/O guard, which records the worker and io_context
    /// generation that own its completion. During shrink, an owner continues
    /// polling until its pending backend operations and active I/O pins have
    /// both reached zero.
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

        // Start the watchdog on the read loop's current worker. Once its timer
        // is pending, operation ownership keeps that backend draining through
        // an autoscaler shrink until completion or cancellation cleanup.
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
    void dispatch_request(coro::task_group& group,
                          frame_header header,
                          message_buffer payload) {
        auto active = std::make_shared<active_request_state>();
        auto lease = std::make_shared<active_request_lease>(
            this, header.request_id, active);

        switch (reserve_active_request(header.request_id, active)) {
            case active_request_reservation::reserved:
                break;

            case active_request_reservation::duplicate:
                ELIO_LOG_WARNING(
                    "RPC session: duplicate active request id {}; closing session",
                    header.request_id);
                close();
                return;

            case active_request_reservation::limited:
                if (config_.request_overload_policy ==
                    rpc_request_overload_policy::reject_request) {
                    reject_overloaded_request(group, header);
                    return;
                }

                ELIO_LOG_WARNING(
                    "RPC session: max_in_flight_requests_per_session={} reached; closing session",
                    config_.max_in_flight_requests_per_session);
                close();
                return;

            case active_request_reservation::closed:
                return;
        }

        auto self = this->shared_from_this();
        group.spawn(&rpc_session::run_request_task,
                    std::move(self), header, std::move(payload),
                    std::move(lease));
    }

    coro::task<void> run_request_task(
        frame_header header,
        message_buffer payload,
        std::shared_ptr<active_request_lease> lease) {
        try {
            co_await handle_request(
                header, std::move(payload), std::move(lease));
        } catch (const std::exception& e) {
            ELIO_LOG_ERROR(
                "RPC session: request task exception: {}", e.what());
        } catch (...) {
            ELIO_LOG_ERROR("RPC session: request task unknown exception");
        }
    }

    /// Handle an incoming request (runs concurrently with the read loop).
    coro::task<void> handle_request(const frame_header& header,
                                    message_buffer payload,
                                    std::shared_ptr<active_request_lease> lease) {
        const auto& active = lease->active;
        const bool no_response =
            has_flag(header.flags, message_flags::no_response);

        // Find handler
        auto it = handlers_->find(header.method_id);
        if (it == handlers_->end()) {
            ELIO_LOG_WARNING("RPC session: method {} not found", header.method_id);
            if (no_response) {
                co_return;
            }
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
        ctx.cancel_token = active->cancel.get_token();

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

        if (no_response) {
            // No response frame will reference the serialized payload, so
            // release response-owned resources as soon as the handler returns.
            if (cleanup_cb) {
                try {
                    cleanup_cb();
                } catch (const std::exception& e) {
                    ELIO_LOG_ERROR("RPC session: cleanup callback exception: {}", e.what());
                }
            }
            co_return;
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
        // sent. Exceptions and false returns from send_response both leave
        // sent == false, so cleanup is skipped per the documented contract.
        if (cleanup_cb && sent) {
            try {
                cleanup_cb();
            } catch (const std::exception& e) {
                ELIO_LOG_ERROR("RPC session: cleanup callback exception: {}", e.what());
            }
        }
    }

    void cancel_request(uint32_t request_id) {
        std::shared_ptr<active_request_state> active;
        {
            std::lock_guard<std::mutex> lock(active_requests_mutex_);
            auto it = active_requests_.find(request_id);
            if (it != active_requests_.end()) {
                active = it->second;
            }
        }

        if (active) {
            cancel_source_noexcept(active->cancel, "request");
        }
    }

    void cancel_all_active_requests() noexcept {
        decltype(active_requests_) active;
        {
            std::lock_guard<std::mutex> lock(active_requests_mutex_);
            active.swap(active_requests_);
        }

        for (auto& [id, request] : active) {
            (void)id;
            cancel_source_noexcept(request->cancel, "request");
        }
    }

    static void cancel_source_noexcept(coro::cancel_source& source,
                                       const char* operation) noexcept {
        try {
            source.cancel();
        } catch (const std::exception& e) {
            try {
                ELIO_LOG_ERROR(
                    "RPC session: {} cancellation callback exception: {}",
                    operation, e.what());
            } catch (...) {
            }
        } catch (...) {
            try {
                ELIO_LOG_ERROR(
                    "RPC session: {} cancellation callback unknown exception",
                    operation);
            } catch (...) {
            }
        }
    }

    void erase_active_request(uint32_t request_id,
                              const std::shared_ptr<active_request_state>& active) noexcept {
        try {
            std::lock_guard<std::mutex> lock(active_requests_mutex_);
            auto it = active_requests_.find(request_id);
            if (it != active_requests_.end() && it->second == active) {
                active_requests_.erase(it);
            }
        } catch (...) {
            // Erase from a noexcept path must not propagate.
        }
    }

    active_request_reservation reserve_active_request(
        uint32_t request_id,
        const std::shared_ptr<active_request_state>& active) {
        std::lock_guard<std::mutex> lock(active_requests_mutex_);
        if (closed_.load(std::memory_order_acquire)) {
            return active_request_reservation::closed;
        }
        if (active_requests_.find(request_id) != active_requests_.end()) {
            return active_request_reservation::duplicate;
        }
        if (has_in_flight_limit() &&
            active_requests_.size() >=
                config_.max_in_flight_requests_per_session) {
            return active_request_reservation::limited;
        }
        active_requests_.emplace(request_id, active);
        return active_request_reservation::reserved;
    }

    void schedule_pong(coro::task_group& group, uint32_t request_id) {
        bool expected = false;
        if (!pong_in_flight_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            ELIO_LOG_WARNING(
                "RPC session: dropping pong {} because another pong is in flight",
                request_id);
            return;
        }

        auto self = this->shared_from_this();
        auto lease = std::make_shared<in_flight_flag_lease>(pong_in_flight_);
        group.spawn(&rpc_session::run_pong_task,
                    std::move(self), request_id, std::move(lease));
    }

    coro::task<void> run_pong_task(
        uint32_t request_id,
        [[maybe_unused]] std::shared_ptr<in_flight_flag_lease> lease) {
        try {
            co_await send_pong_response(request_id);
        } catch (const std::exception& e) {
            ELIO_LOG_ERROR(
                "RPC session: pong send exception: {}", e.what());
        } catch (...) {
            ELIO_LOG_ERROR("RPC session: pong send unknown exception");
        }
    }

    coro::task<void> send_pong_response(uint32_t request_id) {
        auto pong = build_pong(request_id);
        buffer_writer empty;
        (void)co_await send_frame(pong, empty);
    }

    void reject_overloaded_request(coro::task_group& group,
                                   const frame_header& header) {
        if (has_flag(header.flags, message_flags::no_response)) {
            ELIO_LOG_WARNING(
                "RPC session: max_in_flight_requests_per_session={} reached; dropping no_response request {}",
                config_.max_in_flight_requests_per_session,
                header.request_id);
            return;
        }

        bool expected = false;
        if (!overload_reject_in_flight_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            ELIO_LOG_WARNING(
                "RPC session: max_in_flight_requests_per_session={} reached; dropping reject response for request {} because another overload rejection is in flight",
                config_.max_in_flight_requests_per_session,
                header.request_id);
            return;
        }

        ELIO_LOG_WARNING(
            "RPC session: max_in_flight_requests_per_session={} reached; rejecting request {}",
            config_.max_in_flight_requests_per_session,
            header.request_id);

        auto self = this->shared_from_this();
        auto lease = std::make_shared<in_flight_flag_lease>(
            overload_reject_in_flight_);
        group.spawn(&rpc_session::run_overload_rejection_task,
                    std::move(self), header, std::move(lease));
    }

    coro::task<void> run_overload_rejection_task(
        frame_header header,
        [[maybe_unused]] std::shared_ptr<in_flight_flag_lease> lease) {
        try {
            co_await send_overloaded_rejection(header);
        } catch (const std::exception& e) {
            ELIO_LOG_ERROR(
                "RPC session: overload rejection send exception: {}",
                e.what());
        } catch (...) {
            ELIO_LOG_ERROR(
                "RPC session: overload rejection send unknown exception");
        }
    }

    coro::task<void> send_overloaded_rejection(const frame_header& header) {
        auto error_frame = build_error_response(
            header.request_id,
            rpc_error::resource_exhausted,
            "Too many in-flight requests for this session");
        co_await send_response(error_frame.first, error_frame.second);
    }

    bool has_in_flight_limit() const noexcept {
        return config_.max_in_flight_requests_per_session != 0;
    }

    /// Send a response frame; serialised against other concurrent handlers
    /// on the same connection by `send_mutex_`.
    /// @return true if the frame was written successfully, false otherwise.
    coro::task<bool> send_response(const frame_header& header,
                                    const buffer_writer& payload)
    {
        co_return co_await send_frame(header, payload);
    }

    /// Send a frame unless the session has started closing. close() marks the
    /// session closed before requesting stream shutdown, so a frame write must
    /// register itself after taking send_mutex_. If close() races with an
    /// already-registered write, shutdown is deferred until that write leaves
    /// this guarded section instead of shutting the stream down underneath it.
    coro::task<bool> send_frame(const frame_header& header,
                                const buffer_writer& payload)
    {
        if (closed_.load(std::memory_order_acquire)) {
            co_return false;
        }
        auto lock_result = co_await send_mutex_.lock(
            coro::this_coro::cancel_token());
        if (lock_result == coro::cancel_result::cancelled) {
            co_return false;
        }
        sync::lock_guard guard(send_mutex_);
        if (!begin_frame_write()) {
            co_return false;
        }
        frame_write_guard write_guard(this);
        auto write_token = frame_write_cancel_.get_token();
        if (write_token.is_cancelled()) {
            co_return false;
        }
        co_return co_await write_frame(stream_, header, payload, write_token);
    }

    bool begin_frame_write() {
        std::lock_guard<std::mutex> lock(close_mutex_);
        if (closed_.load(std::memory_order_acquire)) {
            return false;
        }
        ++active_frame_writes_;
        return true;
    }

    void finish_frame_write() noexcept {
        bool shutdown_now = false;
        {
            std::lock_guard<std::mutex> lock(close_mutex_);
            if (active_frame_writes_ > 0) {
                --active_frame_writes_;
            }
            if (active_frame_writes_ == 0 && shutdown_deferred_) {
                shutdown_deferred_ = false;
                shutdown_now = true;
            }
        }
        if (shutdown_now) {
            stream_.shutdown_socket();
        }
    }

    void request_stream_shutdown() noexcept {
        bool shutdown_now = false;
        {
            std::lock_guard<std::mutex> lock(close_mutex_);
            if (active_frame_writes_ == 0) {
                shutdown_now = true;
            } else {
                shutdown_deferred_ = true;
            }
        }
        if (shutdown_now) {
            stream_.shutdown_socket();
        }
    }

    Stream stream_;
    /// Snapshot of the server's handler map captured at session creation.
    /// Guarantees the read loop never observes a torn map (Fix 5).
    std::shared_ptr<const handler_map> handlers_;
    rpc_server_config config_;
    std::atomic<bool> closed_{false};
    sync::mutex send_mutex_;
    coro::cancel_source frame_write_cancel_;
    std::mutex close_mutex_;
    size_t active_frame_writes_ = 0;
    bool shutdown_deferred_ = false;
    std::mutex active_requests_mutex_;
    std::unordered_map<uint32_t, std::shared_ptr<active_request_state>> active_requests_;
    std::atomic<bool> overload_reject_in_flight_{false};
    std::atomic<bool> pong_in_flight_{false};
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
            Request request = parse_typed_payload<Request>(payload);

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
            Request request = parse_typed_payload<Request>(payload);

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
            Request request = parse_typed_payload<Request>(payload);

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
    /// The cleanup callback is invoked after a normal response is successfully
    /// sent. For no_response one-way requests, it runs after the handler result
    /// has been serialized and the unsent payload can be discarded.
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
            Request request = parse_typed_payload<Request>(payload);

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
            Request request = parse_typed_payload<Request>(payload);

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
        auto accept_token = reset_accept_cancel_source();
        running_.store(true, std::memory_order_release);

        while (running_.load(std::memory_order_acquire)) {
            auto stream = co_await listener.accept(accept_token);
            if (accept_loop_should_stop(accept_token)) {
                break;
            }
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

            if (!track_session_if_running(session)) {
                session->close();
                release_session_slot();
                break;
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
        auto accept_token = reset_accept_cancel_source();
        running_.store(true, std::memory_order_release);

        while (running_.load(std::memory_order_acquire)) {
            auto stream = co_await listener.accept(accept_token);
            if (accept_loop_should_stop(accept_token)) {
                break;
            }
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

            if (!track_session_if_running(session)) {
                session->close();
                release_session_slot();
                break;
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
        cancel_active_accept();

        std::vector<session_ptr> sessions;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions = sessions_;
        }
        for (auto& session : sessions) {
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

    bool track_session_if_running(const session_ptr& session) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        if (!running_.load(std::memory_order_acquire)) {
            return false;
        }
        sessions_.push_back(session);
        return true;
    }

    coro::cancel_token reset_accept_cancel_source() {
        std::lock_guard<std::mutex> lock(accept_cancel_mutex_);
        accept_cancel_source_ = coro::cancel_source{};
        return accept_cancel_source_.get_token();
    }

    void cancel_active_accept() {
        std::lock_guard<std::mutex> lock(accept_cancel_mutex_);
        accept_cancel_source_.cancel();
    }

    bool accept_loop_should_stop(const coro::cancel_token& accept_token) noexcept {
        if (!running_.load(std::memory_order_acquire) ||
            accept_token.is_cancelled()) {
            running_.store(false, std::memory_order_release);
            return true;
        }
        return false;
    }

    /// Remove a session from the tracking vector using swap-and-pop (O(1)).
    /// Must be called without sessions_mutex_ held.
    void remove_session(const session_ptr& session) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = std::find(sessions_.begin(), sessions_.end(), session);
        if (it != sessions_.end()) {
            if (it != sessions_.end() - 1) {
                *it = std::move(sessions_.back());
            }
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
    mutable std::mutex accept_cancel_mutex_;
    coro::cancel_source accept_cancel_source_;
};

/// Type alias for TCP RPC server
using tcp_rpc_server = rpc_server<net::tcp_stream>;

/// Type alias for UDS RPC server
using uds_rpc_server = rpc_server<net::uds_stream>;

} // namespace elio::rpc
