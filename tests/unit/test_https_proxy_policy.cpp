#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/http/http_client.hpp>

#include <type_traits>

using namespace elio::http;

static_assert(std::same_as<connection, elio::net::stream>);
static_assert(static_cast<int>(client_stage::tls) == 4);
static_assert(static_cast<int>(client_stage::proxy_connect) == 9);
static_assert(static_cast<int>(client_stage::proxy_tls) == 10);

TEST_CASE("HTTPS proxy policies keep proxy and origin verification independent",
          "[http][proxy][tls][policy][issue-1250][http_client_streaming]") {
    const auto verify_origin = GENERATE(false, true);
    const auto verify_proxy = GENERATE(false, true);
    transport_config config;
    config.verify_certificate = verify_origin;
    config.proxy.emplace();
    config.proxy->endpoint = "https://LOCAL%68ost/";
    config.proxy->verify_certificate = verify_proxy;
    bool origin_called = false;
    bool proxy_called = false;
    config.configure_tls = [&](transport_tls_config& policy) {
        origin_called = true;
        CHECK(policy.verify_mode() == (verify_origin ? SSL_VERIFY_PEER : SSL_VERIFY_NONE));
        CHECK(policy.set_alpn_protocols("h2,http/1.1"));
    };
    config.proxy->configure_tls = [&](transport_tls_config& policy) {
        proxy_called = true;
        CHECK(policy.verify_mode() == (verify_proxy ? SSL_VERIFY_PEER : SSL_VERIFY_NONE));
        CHECK_FALSE(policy.set_alpn_protocols("h2"));
        CHECK_FALSE(policy.set_alpn_protocols("h2,http/1.1"));
        CHECK(policy.set_alpn_protocols("http/1.1"));
        CHECK(policy.set_alpn_protocols(""));
        CHECK(policy.set_alpn_protocols("http/1.1"));
    };
    transport owner(config);
    REQUIRE(origin_called);
    REQUIRE(proxy_called);
    auto target = url::parse("https://Origin.Example/");
    REQUIRE(target);
    const auto plan = owner.route_plan_for_test(*target);
    const auto& snapshot = plan.snapshot();
    REQUIRE(snapshot.proxy->secure);
    CHECK(snapshot.proxy->endpoint.host == "localhost");
    CHECK(snapshot.proxy->endpoint.port == 443);
    REQUIRE(snapshot.proxy_tls);
    REQUIRE(snapshot.origin_tls);
    CHECK(snapshot.proxy_tls.get() != snapshot.origin_tls.get());
    CHECK(snapshot.proxy_tls->native_handle() != snapshot.origin_tls->native_handle());
    CHECK(snapshot.proxy_tls->verify_mode() == (verify_proxy ? SSL_VERIFY_PEER : SSL_VERIFY_NONE));
    CHECK(snapshot.origin_tls->verify_mode() == (verify_origin ? SSL_VERIFY_PEER : SSL_VERIFY_NONE));
    REQUIRE(plan.key().hops.size() == 1);
    CHECK(plan.key().hops[0].tls_domain != 0);
    CHECK(plan.key().hops[0].tls_domain != plan.key().origin_tls_domain);
    config.proxy->endpoint = "https://changed.example/";
    config.proxy->verify_certificate = !verify_proxy;
    CHECK(snapshot.proxy->endpoint.host == "localhost");
    CHECK(snapshot.proxy_tls->verify_mode() == (verify_proxy ? SSL_VERIFY_PEER : SSL_VERIFY_NONE));
}

TEST_CASE("HTTPS proxy profiles default to verification and isolate complete security domains",
          "[http][proxy][tls][policy][issue-1250][http_client_streaming]") {
    http_proxy_config defaults;
    CHECK(defaults.verify_certificate);
    transport_config config;
    config.proxy = defaults;
    config.proxy->endpoint = "https://proxy.example:8443/";
    config.proxy->basic_auth = proxy_basic_credentials{"user", "secret"};
    transport first(config), second(config);
    auto target = url::parse("https://origin.example/");
    REQUIRE(target);
    auto one = first.route_plan_for_test(*target);
    auto two = second.route_plan_for_test(*target);
    CHECK(one.key().hops[0].tls_domain != two.key().hops[0].tls_domain);
    CHECK(one.key().origin_tls_domain != two.key().origin_tls_domain);
    CHECK(one.key().hops[0].auth_domain != two.key().hops[0].auth_domain);
    CHECK(one.key() != two.key());
    auto http_target = url::parse("http://origin.example/");
    REQUIRE(http_target);
    auto forward = first.route_plan_for_test(*http_target);
    CHECK(forward.key().mode == detail::route_mode::forward_proxy);
    CHECK(forward.key().origin_tls_domain == 0);
    CHECK(forward.key().hops[0].tls_domain == one.key().hops[0].tls_domain);
}

TEST_CASE("Plain proxy profiles do not invoke or publish a proxy TLS policy",
          "[http][proxy][tls][policy][issue-1250][http_client_streaming]") {
    transport_config config;
    config.proxy.emplace();
    config.proxy->endpoint = "http://proxy.example/";
    bool called = false;
    config.proxy->configure_tls = [&](transport_tls_config&) { called = true; };
    transport owner(config);
    auto target = url::parse("https://origin.example/");
    REQUIRE(target);
    const auto plan = owner.route_plan_for_test(*target);
    CHECK_FALSE(called);
    CHECK_FALSE(plan.snapshot().proxy->secure);
    CHECK_FALSE(plan.snapshot().proxy_tls);
    CHECK(plan.key().hops[0].tls_domain == 0);
}

TEST_CASE("HTTPS proxy reference validation rejects widened or unsafe authentication names",
          "[http][proxy][tls][policy][issue-1250][http_client_streaming]") {
    const auto endpoint = GENERATE("https://.example/", "https://%2Eexample/",
        "https://proxy%00.example/", "https://proxy%2Fexample/",
        "https://user:secret@proxy.example/", "https://proxy.example/path");
    http_proxy_config config;
    config.endpoint = endpoint;
    CHECK_THROWS_AS(detail::freeze_proxy_profile(config), std::invalid_argument);
}
