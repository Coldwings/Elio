#include <catch2/catch_test_macros.hpp>
#include <elio/http/detail/request_wire.hpp>

using namespace elio::http;

namespace {

auto wire_profile() {
    http_proxy_config config;
    config.endpoint = "http://proxy.example:8080";
    config.basic_auth = proxy_basic_credentials{"user", "password"};
    return detail::freeze_proxy_profile(config);
}

} // namespace

TEST_CASE("HTTP forwarding projects absolute form without URI or generic hop secrets",
          "[http][proxy][wire][issue-1249]") {
    const auto parsed = url::parse("http://uri-user:uri-secret@[2001:db8::1]:8081/path?q=1#secret-fragment");
    REQUIRE(parsed);
    request req(method::POST, parsed->path);
    req.set_query(parsed->query);
    req.set_host("wrong-origin.example");
    req.set_header("pRoXy-aUtHoRiZaTiOn", "Basic caller-secret");
    req.set_header("Authorization", "Bearer origin-secret");
    req.set_body(std::string(65536, 'x'));
    req.set_expect_continue();
    const auto original = req.serialize_headers();
    const auto body_address = req.body().data();
    auto profile = wire_profile();
    const auto wire = detail::request_wire_view::serialize(req, *parsed,
        detail::route_mode::forward_proxy, profile.get());
    REQUIRE(wire.starts_with("POST http://[2001:db8::1]:8081/path?q=1 HTTP/1.1\r\n"));
    REQUIRE(wire.find("Host: [2001:db8::1]:8081\r\n") != std::string::npos);
    REQUIRE(wire.find(profile->authorization) != std::string::npos);
    REQUIRE(wire.find("Bearer origin-secret") != std::string::npos);
    REQUIRE(wire.find("Expect: 100-continue\r\n") != std::string::npos);
    REQUIRE(wire.find("caller-secret") == std::string::npos);
    REQUIRE(wire.find("uri-user") == std::string::npos);
    REQUIRE(wire.find("uri-secret") == std::string::npos);
    REQUIRE(wire.find("secret-fragment") == std::string::npos);
    REQUIRE(req.serialize_headers() == original);
    REQUIRE(req.body().data() == body_address);
}

TEST_CASE("Direct and CONNECT inner requests strip generic proxy authorization only",
          "[http][proxy][wire][issue-1249]") {
    const auto parsed = url::parse("https://origin.example/path");
    REQUIRE(parsed);
    request req(method::GET, "/path");
    req.set_host("caller-origin.example");
    req.set_header("Proxy-Authorization", "Basic caller-secret");
    req.set_header("Authorization", "Bearer origin-secret");
    req.set_expect_continue();
    auto profile = wire_profile();
    for (const auto mode : {detail::route_mode::direct, detail::route_mode::connect_tunnel}) {
        const auto wire = detail::request_wire_view::serialize(req, *parsed, mode, profile.get());
        REQUIRE(wire.starts_with("GET /path HTTP/1.1\r\n"));
        REQUIRE(wire.find("Host: caller-origin.example\r\n") != std::string::npos);
        REQUIRE(wire.find("Bearer origin-secret") != std::string::npos);
        REQUIRE(wire.find("Proxy-Authorization") == std::string::npos);
        REQUIRE(wire.find("caller-secret") == std::string::npos);
        REQUIRE(wire.find(profile->authorization) == std::string::npos);
        REQUIRE(wire.find("Expect:") == std::string::npos);
    }
    REQUIRE(req.header("Proxy-Authorization") == "Basic caller-secret");
}

TEST_CASE("HTTP forward server-wide OPTIONS preserves asterisk form",
          "[http][proxy][wire][issue-1249]") {
    const auto parsed = url::parse("http://origin.example:80/ignored");
    REQUIRE(parsed);
    request req(method::OPTIONS, "*");
    auto profile = wire_profile();
    const auto wire = detail::request_wire_view::serialize(req, *parsed,
        detail::route_mode::forward_proxy, profile.get());
    REQUIRE(wire.starts_with("OPTIONS * HTTP/1.1\r\n"));
    REQUIRE(wire.find("Host: origin.example\r\n") != std::string::npos);
}

TEST_CASE("HTTP forward projection rejects alternate authority or fragment paths",
          "[http][proxy][wire][issue-1249]") {
    const auto parsed = url::parse("http://origin.example/");
    REQUIRE(parsed);
    auto profile = wire_profile();
    for (const auto path : {"http://other.example/", "other.example:80", "*", "/path#secret"}) {
        request req(method::GET, path);
        REQUIRE_THROWS_AS(detail::request_wire_view::serialize(req, *parsed,
            detail::route_mode::forward_proxy, profile.get()), std::invalid_argument);
    }
    request req(method::GET, "/encoded%23fragment");
    req.set_query("q=%23");
    REQUIRE(detail::request_wire_view::serialize(req, *parsed,
        detail::route_mode::forward_proxy, profile.get()).starts_with(
            "GET http://origin.example/encoded%23fragment?q=%23 HTTP/1.1\r\n"));
    req.set_query("q=#secret");
    REQUIRE_THROWS_AS(detail::request_wire_view::serialize(req, *parsed,
        detail::route_mode::forward_proxy, profile.get()), std::invalid_argument);
    REQUIRE_THROWS_AS(detail::request_wire_view::serialize(req, *parsed,
        detail::route_mode::forward_proxy, nullptr), std::invalid_argument);
}
