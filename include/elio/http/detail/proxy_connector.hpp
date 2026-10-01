#pragma once

#include <elio/http/client_base.hpp>
#include <elio/http/detail/proxy_connect.hpp>
#include <elio/http/detail/route_connection.hpp>
#include <elio/http/detail/route_operation.hpp>

#include <optional>
#include <utility>

namespace elio::http::detail {

#ifdef ELIO_RUNTIME_TEST_HOOKS
inline std::atomic<void (*)(connect_tls_stream&)> tunnel_ready_for_test{nullptr};
inline std::atomic<void (*)(connect_tls_stream&, int)> tunnel_handshake_failed_for_test{nullptr};
#endif

inline coro::task<client_result<route_connection>> finish_proxy_setup(
        net::stream& established, const route_plan& plan, coro::cancel_token token,
        std::optional<std::chrono::steady_clock::time_point> deadline,
        client_stage& stage, bounded_pool<route_connection>::permit& capacity,
        std::shared_ptr<void> operation) {
    const auto& profile = *plan.snapshot().proxy;
    stage = client_stage::proxy_connect;
    auto negotiated = co_await negotiate_connect(established, plan.target(), profile, token, deadline);
    if (const auto* error = std::get_if<client_error>(&negotiated)) co_return *error;
    stage = client_stage::tls;
    auto prefix = std::move(std::get<std::vector<char>>(negotiated));
    auto retirement = std::make_shared<route_retirement>();
    retirement->operation = std::move(operation);
    retirement->capacity = std::move(capacity);
    connect_channel lower(std::move(established.as_tcp()), std::move(prefix),
                          profile.limits.max_read_ahead, retirement);
    connect_tls_stream inner(std::move(lower), *plan.snapshot().origin_tls);
    inner.set_hostname(proxy_origin_tls_name(plan.target().host));
#ifdef ELIO_RUNTIME_TEST_HOOKS
    if (auto hook = tls_setup_entered_for_test.load(std::memory_order_acquire)) hook();
#endif
    const auto handshake = co_await inner.handshake(token);
    const auto error = handshake ? 0 : (errno ? errno : EIO);
    if (!handshake) {
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = tunnel_handshake_failed_for_test.load(std::memory_order_acquire))
            hook(inner, error);
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
    if (auto hook = tunnel_ready_for_test.load(std::memory_order_acquire)) hook(inner);
#endif
    co_return route_connection(std::move(inner), std::move(retirement));
}

inline coro::task<client_result<route_connection>> connect_proxy_route(
        const route_plan& plan, std::chrono::nanoseconds connect_timeout,
        coro::cancel_token token,
        std::optional<std::chrono::steady_clock::time_point> acquisition_deadline,
        bounded_pool<route_connection>::permit& capacity, std::shared_ptr<void> operation) {
    const auto& snapshot = plan.snapshot();
    if (!snapshot.proxy || !snapshot.origin_tls || plan.key().hops.size() != 1 ||
        plan.key().protocol != route_protocol::http1 ||
        plan.key().target_dns != route_dns_mode::proxy)
        co_return make_client_error(ENOTSUP, client_stage::acquire);
    const auto& endpoint = snapshot.proxy->endpoint;
    std::optional<std::chrono::steady_clock::time_point> setup_deadline;
    // Only the proxy is resolved locally; it owns target-name resolution.
    auto connected = co_await client_connect_result_impl(endpoint.host, endpoint.port,
        false, nullptr, snapshot.resolve_options, snapshot.rotate_resolved_addresses,
        connect_timeout, token, snapshot.dns_timeout, snapshot.dns_domain,
        acquisition_deadline, &setup_deadline);
    if (const auto* error = std::get_if<client_error>(&connected)) co_return *error;
    auto stream = std::move(std::get<net::stream>(connected));
    if (token.is_cancelled()) co_return make_client_error(ECANCELED, client_stage::connect);
    if (setup_deadline && *setup_deadline <= std::chrono::steady_clock::now())
        co_return make_client_error(ETIMEDOUT, client_stage::connect);
    if (plan.key().mode == route_mode::forward_proxy)
        co_return route_connection(std::move(stream));
    if (plan.key().mode != route_mode::connect_tunnel || !plan.key().target_secure)
        co_return make_client_error(ENOTSUP, client_stage::acquire);
    client_stage stage = client_stage::proxy_connect;
    auto result = co_await await_route_operation<client_result<route_connection>>(
        [&](coro::cancel_token stopped) {
            return finish_proxy_setup(stream, plan, std::move(stopped), setup_deadline,
                                      stage, capacity, operation);
        }, token, setup_deadline);
    if (result.timed_out) co_return make_client_error(ETIMEDOUT, stage);
    if (token.is_cancelled()) co_return make_client_error(ECANCELED, stage);
    if (auto* connection = std::get_if<route_connection>(&result.value)) {
        if (auto anchor = connection->retirement_anchor())
            capacity = std::move(anchor->capacity);
    }
    co_return std::move(result.value);
}

} // namespace elio::http::detail
