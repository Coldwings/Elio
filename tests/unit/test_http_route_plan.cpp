#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <elio/http/http_client.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace elio::http;
using namespace elio::http::detail;
using elio::coro::task;

namespace {

route_plan plan_for(std::string_view input, const route_snapshot& policy) {
    auto target = url::parse(input);
    REQUIRE(target);
    return route_plan(*target, std::make_shared<const route_snapshot>(policy));
}

template<typename T>
T immediate(task<T> operation) {
    auto handle = elio::coro::detail::task_access::handle(operation);
    handle.resume();
    REQUIRE(handle.done());
    return operation.await_resume();
}

struct socket_pair {
    elio::net::tcp_stream client{-1};
    elio::net::tcp_stream peer{-1};
    socket_pair() {
        std::array<int, 2> descriptors{};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                         0, descriptors.data()) != 0)
            throw std::runtime_error("socketpair failed");
        client = elio::net::tcp_stream(descriptors[0]);
        peer = elio::net::tcp_stream(descriptors[1]);
    }
};

struct collision_guard {
    collision_guard() { force_connection_key_collision_for_test.store(true); }
    ~collision_guard() { force_connection_key_collision_for_test.store(false); }
};

struct dial_observation {
    elio::sync::event entered;
    elio::sync::event proceed;
    std::vector<connection> connections;
    std::vector<connection_key> keys;
    std::vector<std::chrono::nanoseconds> dns_timeouts;
    std::vector<bool> rotation;
};

std::atomic<dial_observation*> observing_dial{nullptr};

task<client_result<connection>> controlled_dial(const route_plan& plan,
        std::chrono::nanoseconds, elio::coro::cancel_token token) {
    auto& observation = *observing_dial.load();
    observation.keys.push_back(plan.key());
    const auto index = observation.keys.size() - 1;
    if (index == 0) {
        observation.entered.set();
        if (co_await observation.proceed.wait(token) == elio::coro::cancel_result::cancelled)
            co_return make_client_error(ECANCELED, client_stage::acquire);
    }
    // Observe again after suspension, so a borrowed mutable snapshot fails.
    observation.dns_timeouts.push_back(plan.snapshot().dns_timeout);
    observation.rotation.push_back(plan.snapshot().rotate_resolved_addresses);
    if (index >= observation.connections.size())
        co_return make_client_error(EAGAIN, client_stage::connect);
    co_return std::move(observation.connections[index]);
}

struct dial_guard {
    explicit dial_guard(dial_observation& observation) {
        observing_dial.store(&observation);
        route_connect_for_test.store(controlled_dial);
    }
    ~dial_guard() {
        route_connect_for_test.store(nullptr);
        observing_dial.store(nullptr);
    }
};

task<std::string> read_headers(elio::net::tcp_stream& peer,
                               elio::coro::cancel_token token) {
    std::string result;
    std::array<char, 512> bytes{};
    while (result.find("\r\n\r\n") == std::string::npos && result.size() < 8192) {
        const auto read = co_await peer.read(bytes.data(), bytes.size(), token);
        if (read.result <= 0) break;
        result.append(bytes.data(), static_cast<size_t>(read.result));
    }
    co_return result;
}

} // namespace

TEST_CASE("HTTP route identity normalizes authorities without retaining URL secrets",
          "[http][route][issue-1246]") {
    const route_snapshot policy;
    const auto first = plan_for("HTTP://user:secret@EXAMPLE.test/a?token=private#fragment", policy);
    const auto second = plan_for("http://example.test:80/b", policy);
    REQUIRE(first.key() == second.key());
    REQUIRE(connection_key_hash{}(first.key()) == connection_key_hash{}(second.key()));
    REQUIRE(first.target().authority() == "example.test:80");
    REQUIRE(first.key() != plan_for("https://example.test/", policy).key());
    REQUIRE(first.key() != plan_for("http://example.test:81/", policy).key());
    REQUIRE(first.key() != plan_for("http://example.test./", policy).key());

    const auto v6 = plan_for("https://[2001:0DB8:0:0:0:0:0:1]:443/a", policy);
    REQUIRE(v6.key() == plan_for("https://[2001:db8::1]/b", policy).key());
    REQUIRE(v6.target().authority() == "[2001:db8::1]:443");
    REQUIRE(route_endpoint::from("fe80:0:0:0:0:0:0:1%ethA", 80) ==
            route_endpoint::from("fe80::1%ethA", 80));
    REQUIRE(route_endpoint::from("fe80::1%ethA", 80) !=
            route_endpoint::from("fe80::1%etha", 80));
}

TEST_CASE("HTTP route compatibility distinguishes proxy and layered security domains",
          "[http][route][issue-1246]") {
    route_snapshot direct;
    route_snapshot forward = direct;
    forward.mode = route_mode::forward_proxy;
    forward.hops.push_back({route_endpoint::from("PROXY.test", 8080),
                            0, new_route_domain()});
    auto secure_forward = forward;
    secure_forward.hops[0].tls_domain = new_route_domain();
    auto tunnel = forward;
    tunnel.mode = route_mode::connect_tunnel;
    auto secure_tunnel = tunnel;
    secure_tunnel.hops[0].tls_domain = secure_forward.hops[0].tls_domain;
    auto alias = forward;
    alias.hops[0].endpoint.host = "PrOxY.TeSt";
    REQUIRE(plan_for("http://origin.test/", forward).key() ==
            plan_for("http://origin.test/", alias).key());

    const std::array plans{
        plan_for("http://origin.test/", direct),
        plan_for("https://origin.test/", direct),
        plan_for("http://origin.test/", forward),
        plan_for("http://origin.test/", secure_forward),
        plan_for("https://origin.test/", tunnel),
        plan_for("https://origin.test/", secure_tunnel)};
    for (size_t i = 0; i < plans.size(); ++i) {
        for (size_t j = 0; j < plans.size(); ++j) {
            INFO("route coordinates " << i << ", " << j);
            REQUIRE((plans[i].key() == plans[j].key()) == (i == j));
        }
    }
    // Forward channels are intentionally partitioned by origin in this stage.
    REQUIRE(plans[2].key() != plan_for("http://other.test/", forward).key());
    REQUIRE(plans[4].key() != plan_for("https://other.test/", tunnel).key());

    const auto change = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
    auto changed = secure_tunnel;
    switch (change) {
    case 0: changed.hops[0].endpoint.host = "another-proxy.test"; break;
    case 1: changed.hops[0].endpoint.port = 8443; break;
    case 2: changed.hops[0].auth_domain = new_route_domain(); break;
    case 3: changed.hops[0].tls_domain = new_route_domain(); break;
    case 4: changed.origin_tls_domain = new_route_domain(); break;
    case 5: changed.connector_domain = new_route_domain(); break;
    case 6: changed.resolution_domain = new_route_domain(); break;
    case 7: changed.protocol = route_protocol::http2; break;
    case 8: changed.target_dns = route_dns_mode::proxy; break;
    case 9: changed.hops.push_back(changed.hops.front()); break;
    }
    REQUIRE(plans[5].key() != plan_for("https://origin.test/", changed).key());
    auto ordered = changed;
    ordered.hops.push_back({route_endpoint::from("second.test", 8080), 0, 0});
    auto reversed = ordered;
    std::reverse(reversed.hops.begin(), reversed.hops.end());
    REQUIRE(plan_for("https://origin.test/", ordered).key() !=
            plan_for("https://origin.test/", reversed).key());
}

TEST_CASE("HTTP pool compares complete route keys under deliberate hash collisions",
          "[http][route][pool][issue-1246]") {
    collision_guard collisions;
    route_snapshot policy;
    policy.mode = route_mode::connect_tunnel;
    policy.hops.push_back({route_endpoint::from("proxy.test", 8080),
                           new_route_domain(), new_route_domain()});
    auto other_policy = policy;
    other_policy.hops[0].auth_domain = new_route_domain();
    auto first = plan_for("https://origin.test/first", policy);
    auto second = plan_for("https://origin.test/second", other_policy);
    auto absent = plan_for("https://other.test/", policy);
    REQUIRE(connection_key_hash{}(first.key()) == connection_key_hash{}(second.key()));
    REQUIRE(first.key() != second.key());
    socket_pair one;
    socket_pair two;
    socket_pair legacy;
    const auto one_fd = one.client.fd();
    const auto two_fd = two.client.fd();
    const auto legacy_fd = legacy.client.fd();
    connection_pool pool;
    pool.release_plan_for_test(first, connection(std::move(one.client)));
    pool.release_plan_for_test(second, connection(std::move(two.client)));
    pool.release("origin.test", 443, true, connection(std::move(legacy.client)));
    elio::coro::cancel_source stopped;
    stopped.cancel();
    auto miss = immediate(pool.acquire_plan_for_test(absent, stopped.get_token()));
    REQUIRE(std::holds_alternative<client_error>(miss));
    REQUIRE(std::get<client_error>(miss).code.value() == ECANCELED);
    // Transport acquisition checks cancellation even for an idle match; the
    // public standalone legacy adapter below retains its historical behavior.
    auto stopped_match = immediate(pool.acquire_plan_for_test(second, stopped.get_token()));
    REQUIRE(std::holds_alternative<client_error>(stopped_match));
    REQUIRE(std::get<client_error>(stopped_match).code.value() == ECANCELED);
    auto two_result = immediate(pool.acquire_plan_for_test(second));
    REQUIRE(std::holds_alternative<connection>(two_result));
    REQUIRE(std::get<connection>(two_result).fd() == two_fd);
    auto one_result = immediate(pool.acquire_plan_for_test(first));
    REQUIRE(std::holds_alternative<connection>(one_result));
    REQUIRE(std::get<connection>(one_result).fd() == one_fd);
    auto isolated = immediate(pool.acquire_plan_for_test(first, stopped.get_token()));
    REQUIRE(std::holds_alternative<client_error>(isolated));
    REQUIRE(std::get<client_error>(isolated).code.value() == ECANCELED);
    auto legacy_result = immediate(pool.acquire_result("origin.test", 443, true,
                                                       nullptr, {}, stopped.get_token()));
    REQUIRE(std::holds_alternative<connection>(legacy_result));
    REQUIRE(std::get<connection>(legacy_result).fd() == legacy_fd);
}

TEST_CASE("HTTP route plans retain their frozen owners and omit non-compatibility policy",
          "[http][route][issue-1246]") {
    std::optional<route_plan> retained;
    std::weak_ptr<const route_snapshot> snapshot;
    std::weak_ptr<elio::net::resolve_domain> dns;
    std::weak_ptr<elio::tls::tls_context> tls;
    {
        route_snapshot builder;
        builder.dns_domain = std::make_shared<elio::net::resolve_domain>(3);
        builder.origin_tls = std::make_shared<elio::tls::tls_context>(elio::tls::tls_mode::client);
        builder.dns_timeout = std::chrono::milliseconds(10);
        auto published = std::make_shared<const route_snapshot>(builder);
        dns = builder.dns_domain;
        tls = builder.origin_tls;
        snapshot = published;
        auto target = url::parse("http://user:secret@EXAMPLE.test/private?token=private");
        REQUIRE(target);
        retained.emplace(*target, published);
    }
    REQUIRE_FALSE(snapshot.expired());
    REQUIRE_FALSE(dns.expired());
    REQUIRE_FALSE(tls.expired());
    REQUIRE(retained->target().host == "example.test");
    retained.reset();
    REQUIRE(snapshot.expired());
    REQUIRE(dns.expired());
    REQUIRE(tls.expired());

    auto owner = std::make_shared<transport>();
    auto first = owner->route_plan_for_test(*url::parse("https://origin.test/a"));
    auto second = owner->route_plan_for_test(*url::parse("https://origin.test/b"));
    REQUIRE(first.key() == second.key());
    auto replacement = std::make_shared<transport>();
    REQUIRE(first.key() != replacement->route_plan_for_test(
        *url::parse("https://origin.test/a")).key());
}

TEST_CASE("HTTP route identity modeling does not enable an unsupported connector",
          "[http][route][issue-1246]") {
    route_snapshot policy;
    policy.mode = route_mode::forward_proxy;
    policy.hops.push_back({route_endpoint::from("proxy.invalid", 8080), 0, 0});
    connection_pool pool;
    auto result = immediate(pool.acquire_plan_for_test(plan_for("http://origin.invalid/", policy)));
    REQUIRE(std::holds_alternative<client_error>(result));
    REQUIRE(std::get<client_error>(result).code.value() == ENOTSUP);
    REQUIRE(std::get<client_error>(result).stage == client_stage::acquire);
}

TEST_CASE("HTTP redirects and suspended dials retain one transport route snapshot",
          "[http][route][client][issue-1246]") {
    const auto backend = GENERATE(elio::io::io_context::backend_type::epoll,
                                 elio::io::io_context::backend_type::io_uring);
#if ELIO_HAS_IO_URING
    if (backend == elio::io::io_context::backend_type::io_uring &&
        !elio::io::io_uring_backend::is_available()) SKIP("io_uring unavailable");
#else
    if (backend == elio::io::io_context::backend_type::io_uring) SKIP("io_uring not compiled");
#endif
    struct backend_guard {
        elio::io::io_context::backend_type old;
        ~backend_guard() { elio::runtime::detail::worker_io_backend_for_test.store(old); }
    } restore{elio::runtime::detail::worker_io_backend_for_test.exchange(backend)};
    socket_pair origin;
    socket_pair redirected;
    dial_observation observation;
    observation.connections.emplace_back(std::move(origin.client));
    observation.connections.emplace_back(std::move(redirected.client));
    dial_guard dial(observation);
    transport_config config;
    config.rotate_resolved_addresses = false;
    config.dns_timeout = std::chrono::milliseconds(7);
    auto published = std::make_shared<transport>(config);
    const auto original = published->route_plan_for_test(*url::parse("http://origin.invalid/")).key();
    client old_client(published);
    elio::coro::cancel_source stop;
    elio::runtime::scheduler scheduler(2);
    scheduler.start();
    std::array<std::string, 4> requests;
    std::array<int, 3> statuses{};
    int replacement_error = 0;
    std::atomic<bool> finished{false};
    auto origin_server = scheduler.go_joinable([&]() -> task<void> {
        requests[0] = co_await read_headers(origin.peer, stop.get_token());
        std::string redirect = "HTTP/1.1 302 Found\r\nContent-Length: 0\r\n"
            "Location: http://redirect.invalid/next\r\nConnection: keep-alive\r\n\r\n";
        co_await origin.peer.write_exactly(redirect, stop.get_token());
        requests[2] = co_await read_headers(origin.peer, stop.get_token());
        std::string success = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
            "Connection: keep-alive\r\n\r\nok";
        co_await origin.peer.write_exactly(success, stop.get_token());
    });
    auto redirect_server = scheduler.go_joinable([&]() -> task<void> {
        const std::string success = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
            "Connection: keep-alive\r\n\r\nok";
        for (size_t i = 0; i < 2; ++i) {
            requests[1 + 2 * i] = co_await read_headers(redirected.peer, stop.get_token());
            co_await redirected.peer.write_exactly(success, stop.get_token());
        }
    });
    auto controller = scheduler.go_joinable([&]() -> task<void> {
        if (co_await observation.entered.wait(stop.get_token()) ==
                elio::coro::cancel_result::cancelled) co_return;
        config.rotate_resolved_addresses = true;
        config.dns_timeout = std::chrono::milliseconds(19);
        published = std::make_shared<transport>(config);
        observation.proceed.set();
    });
    auto requestor = scheduler.go_joinable([&]() -> task<void> {
        auto first = co_await old_client.get_result("http://origin.invalid/first", stop.get_token());
        if (auto* response = std::get_if<elio::http::response>(&first)) statuses[0] = response->status_code();
        auto again = co_await old_client.get_result("http://origin.invalid/again", stop.get_token());
        if (auto* response = std::get_if<elio::http::response>(&again)) statuses[1] = response->status_code();
        auto last = co_await old_client.get_result("http://redirect.invalid/again", stop.get_token());
        if (auto* response = std::get_if<elio::http::response>(&last)) statuses[2] = response->status_code();
        client replacement(published);
        auto changed = co_await replacement.get_result("http://origin.invalid/new", stop.get_token());
        if (auto* error = std::get_if<client_error>(&changed)) replacement_error = error->code.value();
        finished.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    stop.cancel();
    observation.proceed.set();
    requestor.wait_destroyed();
    controller.wait_destroyed();
    origin_server.wait_destroyed();
    redirect_server.wait_destroyed();
    scheduler.shutdown();
    requestor.await_resume();
    controller.await_resume();
    origin_server.await_resume();
    redirect_server.await_resume();
    REQUIRE(finished.load());
    REQUIRE(statuses == std::array<int, 3>{200, 200, 200});
    REQUIRE(replacement_error == EAGAIN);
    REQUIRE(observation.keys.size() == 3);
    REQUIRE(observation.keys[0] == original);
    REQUIRE(observation.keys[1].target.host == "redirect.invalid");
    REQUIRE(observation.keys[1].connector_domain == original.connector_domain);
    REQUIRE(observation.keys[1].resolution_domain == original.resolution_domain);
    REQUIRE(observation.keys[2].connector_domain != original.connector_domain);
    REQUIRE(observation.keys[2].resolution_domain != original.resolution_domain);
    REQUIRE(observation.dns_timeouts == std::vector<std::chrono::nanoseconds>{
        std::chrono::milliseconds(7), std::chrono::milliseconds(7), std::chrono::milliseconds(19)});
    REQUIRE(observation.rotation == std::vector<bool>{false, false, true});
    REQUIRE(requests[0].starts_with("GET /first HTTP/1.1\r\n"));
    REQUIRE(requests[1].starts_with("GET /next HTTP/1.1\r\n"));
    REQUIRE(requests[2].starts_with("GET /again HTTP/1.1\r\n"));
    REQUIRE(requests[3].starts_with("GET /again HTTP/1.1\r\n"));
    REQUIRE(requests[0].find("Host: origin.invalid\r\n") != std::string::npos);
    REQUIRE(requests[1].find("Host: redirect.invalid\r\n") != std::string::npos);
}
