#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/http/detail/proxy_profile.hpp>

using namespace elio::http;

TEST_CASE("HTTP proxy profiles own frozen endpoint credentials limits and auth identity",
          "[http][proxy][profile][issue-1249]") {
    http_proxy_config config;
    config.endpoint = "HTTP://PROXY.Example:8080/";
    config.basic_auth = proxy_basic_credentials{"Aladdin", "open sesame"};
    config.connect_limits.max_response_bytes = 1234;
    auto profile = detail::freeze_proxy_profile(config);
    REQUIRE(profile->endpoint.host == "proxy.example");
    REQUIRE(profile->endpoint.port == 8080);
    REQUIRE(profile->authorization == "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==");
    REQUIRE(profile->auth_domain != 0);
    auto second = detail::freeze_proxy_profile(config);
    REQUIRE(second->auth_domain != profile->auth_domain);
    config.endpoint = "http://other.example/";
    config.basic_auth->password = "changed";
    config.connect_limits.max_response_bytes = 9;
    REQUIRE(profile->endpoint.host == "proxy.example");
    REQUIRE(profile->authorization == "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==");
    REQUIRE(profile->limits.max_response_bytes == 1234);
    config.basic_auth.reset();
    auto anonymous = detail::freeze_proxy_profile(config);
    REQUIRE(anonymous->auth_domain == 0);
    REQUIRE(anonymous->authorization.empty());
    config.endpoint = "http://[2001:0db8::1]:8080";
    REQUIRE(detail::freeze_proxy_profile(config)->endpoint.authority() == "[2001:db8::1]:8080");
}

TEST_CASE("HTTP proxy profiles reject unsupported or unsafe endpoint configuration",
          "[http][proxy][profile][issue-1249]") {
    const auto endpoint = GENERATE("", "proxy.example:8080", "http:///", "http://:8080/",
        "socks5://proxy.example:1080", "http://user:secret@proxy.example/",
        "http://proxy.example/path", "http://proxy.example/?token=value",
        "http://proxy.example/#fragment", "http://proxy.example:0/",
        "http://@proxy.example/", "http://proxy.example/?", "http://proxy.example/#",
        "http://proxy%ZZ.example/", "http://proxy%.example/", "http://proxy%2.example/",
        "http://proxy\\host/", "http://[::gg]/", "http://[not-ip]/",
        "http://[v1.name]/", "http://[fe80::1%25]/", "http://[fe80::1%25ethA]/",
        "http://[fe80::1%ethA]/",
        "http://proxy%00.example/", "http://proxy%2Fhost/", "http://proxy%40host/",
        "http://proxy%3Ahost/", "http://proxy%25host/", "http://proxy%5Chost/",
        "http://proxy%C3%A9.example/", "http://proxy%7F.example/", "http://proxy%2Bhost/",
        "http://proxy.example/\r\nInjected: value");
    http_proxy_config config;
    config.endpoint = endpoint;
    REQUIRE_THROWS_AS(detail::freeze_proxy_profile(config), std::invalid_argument);
}

TEST_CASE("HTTP proxy profiles decode local reg-name escapes and normalize IPv6 spelling",
          "[http][proxy][profile][authority][issue-1249]") {
    http_proxy_config config;
    config.endpoint = "http://proxy%41.example:8080/";
    REQUIRE(detail::freeze_proxy_profile(config)->endpoint.host == "proxya.example");
    config.endpoint = "http://local%68ost:8080/";
    REQUIRE(detail::freeze_proxy_profile(config)->endpoint.host == "localhost");
    config.endpoint = "http://%31%32%37.0.0.1/";
    REQUIRE(detail::freeze_proxy_profile(config)->endpoint.host == "127.0.0.1");
    config.endpoint = "http://proxy+host/";
    REQUIRE(detail::freeze_proxy_profile(config)->endpoint.host == "proxy+host");
    config.endpoint = "http://[2001:0DB8:0:0:0:0:0:1]:8080/";
    REQUIRE(detail::freeze_proxy_profile(config)->endpoint.authority() == "[2001:db8::1]:8080");
}

TEST_CASE("HTTP proxy Basic credentials enforce colon control and finite byte bounds",
          "[http][proxy][profile][issue-1249]") {
    REQUIRE_THROWS_AS(detail::proxy_basic_authorization({"user:name", "password"}),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(detail::proxy_basic_authorization({"user\r", "password"}),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(detail::proxy_basic_authorization({"user", "password\n"}),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(detail::proxy_basic_authorization({"user", std::string(4096, 'x')}),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(detail::proxy_basic_authorization({std::string(4096, 'x'), ""}),
                      std::invalid_argument);
    REQUIRE(detail::proxy_basic_authorization({"", ""}) == "Basic Og==");
    REQUIRE(detail::proxy_basic_authorization({"user", "p:ass"}) == "Basic dXNlcjpwOmFzcw==");
    REQUIRE(detail::proxy_basic_authorization({"u", std::string(4094, 'x')}).size() < 5500);
}
