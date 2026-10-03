#pragma once

#include <elio/http/client_base.hpp>
#include <elio/http/detail/proxy_connect.hpp>
#include <elio/http/detail/route_connection.hpp>
#include <elio/http/detail/route_operation.hpp>

#include <exception>
#include <optional>
#include <utility>

namespace elio::http::detail {

#ifdef ELIO_RUNTIME_TEST_HOOKS
inline std::atomic<void (*)(connect_tls_stream&)> tunnel_ready_for_test{nullptr};
inline std::atomic<void (*)(connect_tls_stream&, int)> tunnel_handshake_failed_for_test{nullptr};
inline std::atomic<void (*)(tls::tls_stream&)> proxy_tls_created_for_test{nullptr};
inline std::atomic<void (*)(tls::tls_stream&)> proxy_tls_ready_for_test{nullptr};
inline std::atomic<void (*)(tls::tls_stream&, int)> proxy_tls_handshake_failed_for_test{nullptr};
inline std::atomic<void (*)()> proxy_tls_setup_entered_for_test{nullptr};
inline std::atomic<coro::task<void> (*)(tls::tls_stream&)>
    proxy_tls_before_publish_for_test{nullptr};
inline std::atomic<void (*)()> proxy_tls_route_cleanup_entered_for_test{nullptr};
inline std::atomic<coro::task<void> (*)(secure_proxy_channel&)>
    secure_proxy_tunnel_before_origin_tls_for_test{nullptr};
inline std::atomic<void (*)(secure_proxy_origin_tls_stream&)> nested_tunnel_ready_for_test{nullptr};
inline std::atomic<coro::task<void> (*)(secure_proxy_origin_tls_stream&, coro::cancel_token)>
    nested_tunnel_before_publish_for_test{nullptr};
inline std::atomic<void (*)(secure_proxy_origin_tls_stream&, int)>
    nested_tunnel_handshake_failed_for_test{nullptr};
inline std::atomic<void (*)()> nested_route_departure_cleanup_entered_for_test{nullptr};

inline void observe_proxy_tls_setup() {
    if (auto hook = proxy_tls_setup_entered_for_test.load(std::memory_order_acquire)) hook();
}

inline void observe_proxy_tls_created(tls::tls_stream& stream) {
    if (auto hook = proxy_tls_created_for_test.load(std::memory_order_acquire)) hook(stream);
}

inline void observe_proxy_tls_ready(tls::tls_stream& stream) {
    if (auto hook = proxy_tls_ready_for_test.load(std::memory_order_acquire)) hook(stream);
}

inline void observe_proxy_tls_failure(tls::tls_stream& stream, int error) {
    if (auto hook = proxy_tls_handshake_failed_for_test.load(std::memory_order_acquire))
        hook(stream, error);
}

inline coro::task<void> observe_proxy_tls_before_publish(tls::tls_stream& stream) {
    if (auto hook = proxy_tls_before_publish_for_test.load(std::memory_order_acquire))
        co_await hook(stream);
}
#endif

template<typename Inner>
coro::task<client_result<route_connection>> finish_origin_tls_impl(
        Inner& inner, const route_plan& plan, coro::cancel_token token,
        client_stage& stage, std::shared_ptr<route_retirement> retirement) {
    stage = client_stage::tls;
    inner.set_hostname(proxy_origin_tls_name(plan.target().host));
#ifdef ELIO_RUNTIME_TEST_HOOKS
    if (auto hook = tls_setup_entered_for_test.load(std::memory_order_acquire)) hook();
#endif
    const auto handshake = co_await inner.handshake(token);
    const auto error = handshake ? 0 : (errno ? errno : EIO);
    if (!handshake) {
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if constexpr (std::same_as<Inner, connect_tls_stream>) {
            if (auto hook = tunnel_handshake_failed_for_test.load(std::memory_order_acquire))
                hook(inner, error);
        } else {
            if (auto hook = nested_tunnel_handshake_failed_for_test.load(std::memory_order_acquire))
                hook(inner, error);
        }
#endif
        inner.shutdown_socket();
        co_await inner.abort_and_settle();
        co_return make_client_error(error, client_stage::tls);
    }
    if (!inner.alpn_protocol().empty() && inner.alpn_protocol() != "http/1.1") {
        inner.shutdown_socket();
        co_await inner.abort_and_settle();
        co_return make_client_error(EPROTONOSUPPORT, client_stage::tls);
    }
#ifdef ELIO_RUNTIME_TEST_HOOKS
    if constexpr (std::same_as<Inner, connect_tls_stream>) {
        if (auto hook = tunnel_ready_for_test.load(std::memory_order_acquire)) hook(inner);
        } else {
            if (auto hook = nested_tunnel_before_publish_for_test.load(std::memory_order_acquire))
                co_await hook(inner, token);
            if (auto hook = nested_tunnel_ready_for_test.load(std::memory_order_acquire)) hook(inner);
    }
#endif
    co_return route_connection(std::move(inner), std::move(retirement));
}

template<typename Inner>
coro::task<client_result<route_connection>> finish_origin_tls(
        Inner inner, const route_plan& plan, coro::cancel_token token,
        client_stage& stage, std::shared_ptr<route_retirement> retirement) {
    auto settlement = tls::detail::tls_settlement_access::retain(inner);
    std::optional<client_result<route_connection>> result;
    std::exception_ptr failure;
    try {
        result.emplace(co_await finish_origin_tls_impl(
            inner, plan, std::move(token), stage, std::move(retirement)));
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure) {
        try { co_await settlement.abort_and_settle(); }
        catch (...) {}
        std::rethrow_exception(failure);
    }
    co_return std::move(*result);
}

inline coro::task<client_result<route_connection>> finish_proxy_setup(
        net::stream& established, const route_plan& plan, coro::cancel_token token,
        std::optional<std::chrono::steady_clock::time_point> deadline,
        client_stage& stage, bounded_pool<route_connection>::permit& capacity,
        std::shared_ptr<void> operation) {
    const auto& profile = *plan.snapshot().proxy;
    stage = client_stage::proxy_connect;
    auto negotiated = co_await negotiate_connect(established, plan.wire_target(), profile, token, deadline);
    if (const auto* error = std::get_if<client_error>(&negotiated)) co_return *error;
    auto retirement = std::make_shared<route_retirement>();
    retirement->operation = std::move(operation);
    retirement->capacity = std::move(capacity);
    connect_channel lower(std::move(established.as_tcp()),
        std::move(std::get<std::vector<char>>(negotiated)), profile.limits.max_read_ahead, retirement);
    co_return co_await finish_origin_tls(
        connect_tls_stream(std::move(lower), *plan.snapshot().origin_tls),
        plan, std::move(token), stage, std::move(retirement));
}

inline coro::task<client_result<route_connection>> finish_secure_proxy_setup(
        tls::tls_stream& outer, const route_plan& plan, coro::cancel_token token,
        std::optional<std::chrono::steady_clock::time_point> deadline,
        client_stage& stage, std::shared_ptr<route_retirement> retirement) {
    const auto& profile = *plan.snapshot().proxy;
    stage = client_stage::proxy_connect;
    auto negotiated = co_await negotiate_connect(outer, plan.wire_target(), profile, token, deadline);
    if (const auto* error = std::get_if<client_error>(&negotiated)) {
        co_await outer.abort_and_settle();
        co_return *error;
    }
    // The outer TLS transport retains the physical-root anchor. The inner
    // prefix preserves committed CONNECT read-ahead without duplicating TLS.
    auto outer_settlement = tls::detail::tls_settlement_access::retain(outer);
    std::optional<secure_proxy_origin_tls_stream> inner;
    std::exception_ptr construction_failure;
    try {
        secure_proxy_channel tunnel(std::move(outer),
            std::move(std::get<std::vector<char>>(negotiated)),
            profile.limits.max_read_ahead);
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = secure_proxy_tunnel_before_origin_tls_for_test.load(
                std::memory_order_acquire))
            co_await hook(tunnel);
#endif
        inner.emplace(std::move(tunnel), *plan.snapshot().origin_tls);
    } catch (...) {
        construction_failure = std::current_exception();
    }
    if (construction_failure) {
        try { co_await outer_settlement.abort_and_settle(); }
        catch (...) {}
        std::rethrow_exception(construction_failure);
    }
    co_return co_await finish_origin_tls(std::move(*inner), plan, std::move(token),
                                         stage, std::move(retirement));
}

inline coro::task<void> settle_completed_proxy_route(
        client_result<route_connection>& result) {
    auto* connection = std::get_if<route_connection>(&result);
    if (!connection) co_return;
#ifdef ELIO_RUNTIME_TEST_HOOKS
    if (auto hook = nested_route_departure_cleanup_entered_for_test.load(
            std::memory_order_acquire)) hook();
#endif
    co_await connection->abort_and_settle();
}

inline coro::task<client_result<route_connection>> connect_proxy_route(
        const route_plan& plan, std::chrono::nanoseconds connect_timeout,
        coro::cancel_token token,
        std::optional<std::chrono::steady_clock::time_point> acquisition_deadline,
        bounded_pool<route_connection>::permit& capacity, std::shared_ptr<void> operation) {
    const auto& snapshot = plan.snapshot();
    if (!snapshot.proxy || !snapshot.origin_tls ||
        (snapshot.proxy->secure && !snapshot.proxy_tls) || plan.key().hops.size() != 1 ||
        plan.key().protocol != route_protocol::http1 ||
        plan.key().target_dns != route_dns_mode::proxy)
        co_return make_client_error(ENOTSUP, client_stage::acquire);
    const auto& endpoint = snapshot.proxy->endpoint;
    std::optional<std::chrono::steady_clock::time_point> setup_deadline;
    std::shared_ptr<route_retirement> retirement;
    tls_connect_observer tls_observer;
    if (snapshot.proxy->secure) {
        retirement = std::make_shared<route_retirement>();
        retirement->operation = std::move(operation);
        retirement->capacity = std::move(capacity);
        tls_observer.failure_stage = client_stage::proxy_tls;
#ifdef ELIO_RUNTIME_TEST_HOOKS
        tls_observer.created = observe_proxy_tls_created;
        tls_observer.setup_entered = observe_proxy_tls_setup;
        tls_observer.failed = observe_proxy_tls_failure;
        tls_observer.ready = observe_proxy_tls_ready;
        tls_observer.before_publish = observe_proxy_tls_before_publish;
        tls_observer.replace_default_hooks = true;
#endif
    }
    // Only the proxy is resolved locally; it owns target-name resolution.
    auto connected = co_await client_connect_result_impl(endpoint.host, endpoint.port,
        snapshot.proxy->secure, snapshot.proxy_tls.get(), snapshot.resolve_options,
        snapshot.rotate_resolved_addresses,
        connect_timeout, token, snapshot.dns_timeout, snapshot.dns_domain,
        acquisition_deadline, true, &setup_deadline, retirement, tls_observer);
    if (const auto* error = std::get_if<client_error>(&connected)) co_return *error;
    auto stream = std::move(std::get<net::stream>(connected));
    if (snapshot.proxy->secure) {
        if (!stream.is_tls()) co_return make_client_error(ENOTSUP, client_stage::proxy_tls);
        if (token.is_cancelled() ||
            (setup_deadline && *setup_deadline <= std::chrono::steady_clock::now())) {
            const auto error = token.is_cancelled() ? ECANCELED : ETIMEDOUT;
            co_await stream.as_tls().abort_and_settle();
            co_return make_client_error(error, client_stage::proxy_connect);
        }
        if (!stream.as_tls().alpn_protocol().empty() &&
            stream.as_tls().alpn_protocol() != "http/1.1") {
            co_await stream.as_tls().abort_and_settle();
            co_return make_client_error(EPROTONOSUPPORT, client_stage::proxy_tls);
        }
    } else {
        if (token.is_cancelled()) co_return make_client_error(ECANCELED, client_stage::connect);
        if (setup_deadline && *setup_deadline <= std::chrono::steady_clock::now())
            co_return make_client_error(ETIMEDOUT, client_stage::connect);
    }
    if (plan.key().mode == route_mode::forward_proxy) {
        if (!snapshot.proxy->secure) co_return route_connection(std::move(stream));
        auto result = route_connection(std::move(stream), retirement);
        capacity = std::move(retirement->capacity);
        co_return result;
    }
    if (plan.key().mode != route_mode::forward_proxy &&
        (plan.key().mode != route_mode::connect_tunnel || !plan.key().target_secure))
        co_return make_client_error(ENOTSUP, client_stage::acquire);
    client_stage stage = snapshot.proxy->secure ? client_stage::proxy_tls : client_stage::proxy_connect;
    std::optional<route_operation_result<client_result<route_connection>>> operation_result;
    std::exception_ptr operation_failure;
    try {
        operation_result.emplace(
            co_await await_route_operation<client_result<route_connection>>(
                [&](coro::cancel_token stopped) {
                    if (snapshot.proxy->secure) {
                        return finish_secure_proxy_setup(stream.as_tls(), plan, std::move(stopped),
                                                         setup_deadline, stage, retirement);
                    }
                    return finish_proxy_setup(stream, plan, std::move(stopped), setup_deadline,
                                              stage, capacity, operation);
                }, token, setup_deadline, settle_completed_proxy_route));
    } catch (...) {
        operation_failure = std::current_exception();
    }
    if (operation_failure) {
        if (snapshot.proxy->secure && stream.is_tls()) {
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (auto hook = proxy_tls_route_cleanup_entered_for_test.load(
                    std::memory_order_acquire)) hook();
#endif
            try { co_await stream.as_tls().abort_and_settle(); }
            catch (...) {}
        }
        std::rethrow_exception(operation_failure);
    }
    auto result = std::move(*operation_result);
    const int departure_error = result.timed_out ? ETIMEDOUT :
        token.is_cancelled() ? ECANCELED : 0;
    if (departure_error) {
        // The nested operation can publish a successful route concurrently
        // with its deadline or caller cancellation. Destruction only requests
        // TLS failure; join the recursive lower chain before reporting the
        // already-selected setup outcome and releasing physical capacity.
        try { co_await settle_completed_proxy_route(result.value); }
        catch (...) {}
        co_return make_client_error(departure_error, stage);
    }
    if (auto* connection = std::get_if<route_connection>(&result.value)) {
        if (auto anchor = connection->retirement_anchor())
            capacity = std::move(anchor->capacity);
    }
    co_return std::move(result.value);
}

} // namespace elio::http::detail
