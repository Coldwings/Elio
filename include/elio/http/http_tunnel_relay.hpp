#pragma once
#include <elio/http/http_tunnel.hpp>
#include <elio/net/stream.hpp>
#include <elio/coro/task_group.hpp>
#include <memory>
#include <mutex>

namespace elio::http {
struct relay_options { size_t buffer_size = 64 * 1024; };

namespace detail {
#ifdef ELIO_RUNTIME_TEST_HOOKS
struct tunnel_relay_test_hooks {
    void* context = nullptr;
    coro::task<void> (*before_second_launch)(void*) = nullptr;
    void (*before_direction_start)(void*) = nullptr;
};
inline tunnel_relay_test_hooks* relay_hooks_for_test = nullptr;
#endif
struct tunnel_relay_state {
    std::mutex mutex;
    coro::cancel_source stop;
    tunnel_result result;

    void select(tunnel_end end, int error = 0, bool launch_failure = false) noexcept {
        {
            std::lock_guard lock(mutex);
            if (result.end != tunnel_end::completed &&
                (!launch_failure || (result.end != tunnel_end::cancelled &&
                                     result.end != tunnel_end::session_closed))) return;
            result.end = end;
            result.error = error;
        }
        try { stop.cancel(); } catch (...) {}
    }
    void failure(int error, bool launch_failure = false) noexcept {
        select(error == ECANCELED ? tunnel_end::cancelled :
               error == ETIMEDOUT ? tunnel_end::timed_out : tunnel_end::transport_error,
               error, launch_failure);
    }
};

inline bool add_tunnel_count(uint64_t& total, uint64_t count) noexcept {
    if (count > UINT64_MAX - total) return false;
    total += count;
    return true;
}

// Arguments live in the relay frame, which joins both children on every exit.
inline coro::task<void> relay_direction(tunnel_relay_state* state,
    tunnel_stream* source, tunnel_stream* destination, char* buffer, size_t size,
    tunnel_direction_result* counts) {
    try {
        auto parent_cancel = coro::this_coro::cancel_token().on_cancel(
            [state] { state->failure(ECANCELED); });
        auto token = state->stop.get_token();
        for (;;) {
            auto read = co_await source->read(buffer, size, token);
            if (read.result < 0) { state->failure(-read.result); co_return; }
            if (read.result == 0) {
                if (source->read_end_scope() == net::close_scope::whole_session) {
                    state->select(tunnel_end::session_closed);
                    co_return;
                }
                auto finished = co_await destination->finish_output(token);
                if (finished.error) state->failure(finished.error);
                else if (finished.scope == net::close_scope::whole_session)
                    state->select(tunnel_end::session_closed);
                co_return;
            }
            if (static_cast<size_t>(read.result) > size ||
                !add_tunnel_count(counts->source_bytes, static_cast<uint64_t>(read.result))) {
                state->failure(EOVERFLOW); co_return;
            }
            auto written = co_await destination->write(buffer, static_cast<size_t>(read.result), token);
            // Preserve confirmed late progress before inspecting cancellation.
            if (!add_tunnel_count(counts->accepted_bytes, written.accepted_bytes)) {
                state->failure(EOVERFLOW); co_return;
            }
            counts->uncertain_write = counts->uncertain_write || written.uncertain_attempt;
            if (written.error == ESHUTDOWN &&
                destination->read_end_scope() == net::close_scope::whole_session) {
                // Join the existing TLS close driver before stopping its
                // sibling. Cancellation here would truncate its close alert.
                auto finished = co_await destination->finish_output(token);
                if (finished.error) state->failure(finished.error);
                else state->select(tunnel_end::session_closed);
                co_return;
            }
            if (written.error) { state->failure(written.error); co_return; }
        }
    } catch (const std::bad_alloc&) { state->failure(ENOMEM); }
    catch (...) { state->failure(EIO); }
}

// Invocation happens inside task_group's child owner. A coroutine allocation
// failure here is reported by join(), possibly after sibling cancellation.
inline coro::task<void> start_relay_direction(tunnel_relay_state* state,
    tunnel_stream* source, tunnel_stream* destination, char* buffer, size_t size,
    tunnel_direction_result* counts) {
#ifdef ELIO_RUNTIME_TEST_HOOKS
    if (relay_hooks_for_test && relay_hooks_for_test->before_direction_start)
        relay_hooks_for_test->before_direction_start(relay_hooks_for_test->context);
#endif
    return relay_direction(state, source, destination, buffer, size, counts);
}

template<typename Stream>
coro::task<tunnel_result> relay_with_stream(tunnel_stream& client, Stream& upstream,
                                          relay_options options, coro::cancel_token token) {
    if (!options.buffer_size || options.buffer_size > static_cast<size_t>(INT32_MAX))
        co_return tunnel_result{tunnel_end::invalid_state, EINVAL};
    const auto inherited = coro::this_coro::cancel_token();
    if (token.is_cancelled() || inherited.is_cancelled())
        co_return tunnel_result{tunnel_end::cancelled, ECANCELED};
    tunnel_relay_state state;
    std::unique_ptr<char[]> outbound;
    std::unique_ptr<char[]> inbound;
    std::unique_ptr<tunnel_stream> upstream_view;
    std::unique_ptr<coro::task_group> children;
    coro::cancel_token::registration registration;
    coro::cancel_token::registration inherited_registration;
    try {
        outbound = std::make_unique_for_overwrite<char[]>(options.buffer_size);
        inbound = std::make_unique_for_overwrite<char[]>(options.buffer_size);
        // Guaranteed elision initializes the nonmovable borrowed view in place.
        upstream_view.reset(new tunnel_stream(tunnel_stream_access::create(upstream)));
        registration = token.on_cancel([&state] { state.failure(ECANCELED); });
        inherited_registration = inherited.on_cancel([&state] { state.failure(ECANCELED); });
        children = std::make_unique<coro::task_group>();
        children->spawn(start_relay_direction, &state, &client, upstream_view.get(), outbound.get(),
                        options.buffer_size, &state.result.client_to_upstream);
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (relay_hooks_for_test && relay_hooks_for_test->before_second_launch)
            co_await relay_hooks_for_test->before_second_launch(relay_hooks_for_test->context);
#endif
        children->spawn(start_relay_direction, &state, upstream_view.get(), &client, inbound.get(),
                        options.buffer_size, &state.result.upstream_to_client);
    } catch (const std::bad_alloc&) { state.failure(ENOMEM, true); }
    catch (...) { state.failure(EIO, true); }
    if (children) {
        try { co_await children->join(); }
        // Bodies contain their own I/O failures. A join exception reports a
        // failure before body entry and must not be hidden by fail-fast cleanup.
        catch (const std::bad_alloc&) { state.failure(ENOMEM, true); }
        catch (...) { state.failure(EIO, true); }
    }
    registration.unregister();
    inherited_registration.unregister();
    co_return state.result;
}
} // namespace detail

// Upstream selection, connection establishment and authorization belong to the
// caller. Exactly two fixed payload buffers; no reverse-half-close timer.
inline coro::task<tunnel_result> relay(tunnel_stream& client, net::stream& upstream,
    relay_options options = {}, coro::cancel_token token = {}) {
    try {
        co_return co_await detail::relay_with_stream(client, upstream, options, std::move(token));
    } catch (const std::bad_alloc&) {
        co_return tunnel_result{tunnel_end::transport_error, ENOMEM};
    } catch (...) {
        co_return tunnel_result{tunnel_end::transport_error, EIO};
    }
}
} // namespace elio::http
