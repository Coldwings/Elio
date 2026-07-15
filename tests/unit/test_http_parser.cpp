#include <catch2/catch_test_macros.hpp>
#include <elio/http/http_parser.hpp>
#include <elio/http/http_common.hpp>
#include <elio/http/http_message.hpp>

using namespace elio::http;

TEST_CASE("HTTP request parser - basic GET request", "[http][parser]") {
    request_parser parser;

    std::string request =
        "GET /path/to/resource HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: test/1.0\r\n"
        "\r\n";

    auto [result, consumed] = parser.parse(request);

    REQUIRE(result == parse_result::complete);
    REQUIRE(parser.get_method() == method::GET);
    REQUIRE(parser.path() == "/path/to/resource");
    REQUIRE(parser.version() == "HTTP/1.1");
    REQUIRE(parser.get_headers().get("Host") == "example.com");
    REQUIRE(parser.get_headers().get("User-Agent") == "test/1.0");
    REQUIRE(parser.body().empty());
}

TEST_CASE("HTTP request parser - GET with query string", "[http][parser]") {
    request_parser parser;

    std::string request =
        "GET /search?q=hello&page=1 HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";

    auto [result, consumed] = parser.parse(request);

    REQUIRE(result == parse_result::complete);
    REQUIRE(parser.path() == "/search");
    REQUIRE(parser.query() == "q=hello&page=1");
}

TEST_CASE("HTTP request parser - POST with body", "[http][parser]") {
    request_parser parser;

    std::string request =
        "POST /api/data HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "{\"key\":\"val\"}";

    auto [result, consumed] = parser.parse(request);

    REQUIRE(result == parse_result::complete);
    REQUIRE(parser.get_method() == method::POST);
    REQUIRE(parser.body() == "{\"key\":\"val\"}");
    REQUIRE(parser.get_headers().content_type() == "application/json");
}

TEST_CASE("HTTP request parser - chunked encoding", "[http][parser]") {
    request_parser parser;

    std::string request =
        "POST /upload HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "Hello\r\n"
        "6\r\n"
        " World\r\n"
        "0\r\n"
        "\r\n";

    auto [result, consumed] = parser.parse(request);

    REQUIRE(result == parse_result::complete);
    REQUIRE(parser.body() == "Hello World");
}

TEST_CASE("HTTP request parser - incremental parsing", "[http][parser]") {
    request_parser parser;

    // Send request in chunks
    std::string part1 = "GET /test HTTP/1.1\r\n";
    std::string part2 = "Host: example.com\r\n";
    std::string part3 = "\r\n";

    auto [r1, c1] = parser.parse(part1);
    REQUIRE(r1 == parse_result::need_more);
    REQUIRE_FALSE(parser.is_complete());

    auto [r2, c2] = parser.parse(part2);
    REQUIRE(r2 == parse_result::need_more);
    REQUIRE_FALSE(parser.is_complete());

    auto [r3, c3] = parser.parse(part3);
    REQUIRE(r3 == parse_result::complete);
    REQUIRE(parser.is_complete());
}

TEST_CASE("HTTP request parser - all methods", "[http][parser]") {
    auto test_method = [](const char* method_str, method expected) {
        request_parser parser;
        std::string request = std::string(method_str) + " / HTTP/1.1\r\nHost: test\r\n\r\n";
        auto [result, consumed] = parser.parse(request);
        REQUIRE(result == parse_result::complete);
        REQUIRE(parser.get_method() == expected);
    };

    test_method("GET", method::GET);
    test_method("HEAD", method::HEAD);
    test_method("POST", method::POST);
    test_method("PUT", method::PUT);
    test_method("DELETE", method::DELETE_);
    test_method("PATCH", method::PATCH);
    test_method("OPTIONS", method::OPTIONS);
    test_method("CONNECT", method::CONNECT);
    test_method("TRACE", method::TRACE);
}

TEST_CASE("HTTP request parser - invalid request", "[http][parser]") {
    SECTION("Missing method") {
        request_parser parser;
        auto [result, consumed] = parser.parse("/ HTTP/1.1\r\n\r\n");
        REQUIRE(result == parse_result::error);
        REQUIRE(parser.has_error());
    }

    SECTION("Unknown method") {
        request_parser parser;
        auto [result, consumed] = parser.parse("INVALID / HTTP/1.1\r\n\r\n");
        REQUIRE(result == parse_result::error);
    }

    SECTION("Empty request target") {
        request_parser parser;
        auto [result, consumed] = parser.parse("GET  HTTP/1.1\r\n\r\n");
        REQUIRE(result == parse_result::error);
    }

    SECTION("Invalid HTTP version") {
        request_parser parser;
        auto [result, consumed] = parser.parse("GET / FTP/1.0\r\n\r\n");
        REQUIRE(result == parse_result::error);
    }

    SECTION("Malformed HTTP version") {
        const char* invalid_versions[] = {
            "HTTP/",
            "HTTP/1",
            "HTTP/1.",
            "HTTP/.1",
            "HTTP/one.one",
            "HTTP/1.1 extra",
            "HTTP/11.22",
            "HTTP/1.10",
            "HTTP/10.1",
        };

        for (auto* version : invalid_versions) {
            request_parser parser;
            std::string request =
                std::string("GET / ") + version + "\r\n\r\n";
            auto [result, consumed] = parser.parse(request);
            REQUIRE(result == parse_result::error);
        }
    }

    SECTION("Missing colon in header") {
        request_parser parser;
        auto [result, consumed] = parser.parse("GET / HTTP/1.1\r\nBadHeader\r\n\r\n");
        REQUIRE(result == parse_result::error);
    }
}

TEST_CASE("HTTP request parser - reset", "[http][parser]") {
    request_parser parser;

    std::string request1 = "GET /first HTTP/1.1\r\nHost: test\r\n\r\n";
    auto [r1, c1] = parser.parse(request1);
    REQUIRE(r1 == parse_result::complete);
    REQUIRE(parser.path() == "/first");

    parser.reset();

    std::string request2 = "POST /second HTTP/1.1\r\nHost: test\r\nContent-Length: 4\r\n\r\ntest";
    auto [r2, c2] = parser.parse(request2);
    REQUIRE(r2 == parse_result::complete);
    REQUIRE(parser.path() == "/second");
    REQUIRE(parser.get_method() == method::POST);
    REQUIRE(parser.body() == "test");
}

// Response parser tests

TEST_CASE("HTTP response parser - basic 200 OK", "[http][parser]") {
    response_parser parser;

    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "<html></html>";

    auto [result, consumed] = parser.parse(response);

    REQUIRE(result == parse_result::complete);
    REQUIRE(parser.get_status() == status::ok);
    REQUIRE(parser.status_code() == 200);
    REQUIRE(parser.version() == "HTTP/1.1");
    REQUIRE(parser.reason() == "OK");
    REQUIRE(parser.body() == "<html></html>");
}

TEST_CASE("HTTP response parser - various status codes", "[http][parser]") {
    auto test_status = [](uint16_t code, const char* reason, status expected) {
        response_parser parser;
        std::string response = "HTTP/1.1 " + std::to_string(code) + " " + reason +
            "\r\nContent-Length: 0\r\n\r\n";
        auto [result, consumed] = parser.parse(response);
        REQUIRE(result == parse_result::complete);
        REQUIRE(parser.get_status() == expected);
        REQUIRE(parser.status_code() == code);
    };

    test_status(200, "OK", status::ok);
    test_status(201, "Created", status::created);
    test_status(204, "No Content", status::no_content);
    test_status(301, "Moved Permanently", status::moved_permanently);
    test_status(302, "Found", status::found);
    test_status(400, "Bad Request", status::bad_request);
    test_status(401, "Unauthorized", status::unauthorized);
    test_status(403, "Forbidden", status::forbidden);
    test_status(404, "Not Found", status::not_found);
    test_status(500, "Internal Server Error", status::internal_server_error);
    test_status(503, "Service Unavailable", status::service_unavailable);
}

TEST_CASE("HTTP response parser rejects malformed versions", "[http][parser]") {
    const char* invalid_versions[] = {
        "FTP/1.0",
        "HTTP/",
        "HTTP/1",
        "HTTP/1.",
        "HTTP/.1",
        "HTTP/one.one",
        "HTTP/11.22",
        "HTTP/1.10",
        "HTTP/10.1",
    };

    for (auto* version : invalid_versions) {
        response_parser parser;
        std::string response =
            std::string(version) + " 200 OK\r\nContent-Length: 0\r\n\r\n";
        auto [result, consumed] = parser.parse(response);
        REQUIRE(result == parse_result::error);
    }
}

TEST_CASE("HTTP response parser rejects malformed status codes", "[http][parser]") {
    const char* invalid_codes[] = {
        "",
        "20",
        "2000",
        "302x",
        "30x",
        "x02",
    };

    for (auto* code : invalid_codes) {
        response_parser parser;
        std::string response =
            std::string("HTTP/1.1 ") + code + " Found\r\n"
            "Content-Length: 0\r\n\r\n";
        auto [result, consumed] = parser.parse(response);
        REQUIRE(result == parse_result::error);
    }
}

TEST_CASE("HTTP response parser - chunked response", "[http][parser]") {
    response_parser parser;

    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "7\r\n"
        "Mozilla\r\n"
        "9\r\n"
        "Developer\r\n"
        "7\r\n"
        "Network\r\n"
        "0\r\n"
        "\r\n";

    auto [result, consumed] = parser.parse(response);

    REQUIRE(result == parse_result::complete);
    REQUIRE(parser.body() == "MozillaDeveloperNetwork");
}

TEST_CASE("HTTP response parser - incremental parsing", "[http][parser]") {
    response_parser parser;

    std::string part1 = "HTTP/1.1 200 OK\r\n";
    std::string part2 = "Content-Length: 5\r\n\r\n";
    std::string part3 = "Hello";

    auto [r1, c1] = parser.parse(part1);
    REQUIRE(r1 == parse_result::need_more);

    auto [r2, c2] = parser.parse(part2);
    REQUIRE(r2 == parse_result::need_more);

    auto [r3, c3] = parser.parse(part3);
    REQUIRE(r3 == parse_result::complete);
    REQUIRE(parser.body() == "Hello");
}

TEST_CASE("HTTP response parser - no body", "[http][parser]") {
    response_parser parser;

    std::string response =
        "HTTP/1.1 204 No Content\r\n"
        "Server: test\r\n"
        "\r\n";

    auto [result, consumed] = parser.parse(response);

    REQUIRE(result == parse_result::complete);
    REQUIRE(parser.get_status() == status::no_content);
    REQUIRE(parser.body().empty());
}

TEST_CASE("HTTP response parser - no-body statuses ignore advertised length", "[http][parser]") {
    SECTION("204 completes at the header boundary") {
        response_parser parser;

        std::string response =
            "HTTP/1.1 204 No Content\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello";

        auto [result, consumed] = parser.parse(response);

        REQUIRE(result == parse_result::complete);
        REQUIRE(parser.body().empty());
        REQUIRE(parser.bytes_remaining() == 5);
        REQUIRE(consumed == response.size() - 5);
    }

    SECTION("304 completes at the header boundary") {
        response_parser parser;

        std::string response =
            "HTTP/1.1 304 Not Modified\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello";

        auto [result, consumed] = parser.parse(response);

        REQUIRE(result == parse_result::complete);
        REQUIRE(parser.status_code() == 304);
        REQUIRE(parser.body().empty());
        REQUIRE(parser.bytes_remaining() == 5);
        REQUIRE(consumed == response.size() - 5);
    }
}

TEST_CASE("HTTP response parser - HEAD response has no body", "[http][parser]") {
    response_parser parser;
    parser.set_request_method(method::HEAD);

    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    auto [result, consumed] = parser.parse(response);

    REQUIRE(result == parse_result::complete);
    REQUIRE(parser.status_code() == 200);
    REQUIRE(parser.body().empty());
    REQUIRE(parser.bytes_remaining() == 5);
    REQUIRE(consumed == response.size() - 5);
}

TEST_CASE("HTTP response parser - close-delimited body completes on EOF", "[http][parser]") {
    response_parser parser;

    std::string first =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "\r\n"
        "hello";

    auto [r1, c1] = parser.parse(first);
    REQUIRE(r1 == parse_result::need_more);
    REQUIRE(c1 == first.size());
    REQUIRE(parser.is_close_delimited());
    REQUIRE(parser.body() == "hello");
    REQUIRE_FALSE(parser.is_complete());

    auto [r2, c2] = parser.parse(" world");
    REQUIRE(r2 == parse_result::need_more);
    REQUIRE(c2 == 6);
    REQUIRE(parser.body() == "hello world");

    auto [r3, c3] = parser.finish_eof();
    REQUIRE(r3 == parse_result::complete);
    REQUIRE(c3 == 0);
    REQUIRE(parser.is_complete());
    REQUIRE(parser.body() == "hello world");
}

TEST_CASE("HTTP response parser - EOF before framed body completes is an error", "[http][parser]") {
    response_parser parser;

    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "he";

    auto [r1, c1] = parser.parse(response);
    REQUIRE(r1 == parse_result::need_more);
    REQUIRE(parser.body() == "he");

    auto [r2, c2] = parser.finish_eof();
    REQUIRE(r2 == parse_result::error);
    REQUIRE(c2 == 0);
    REQUIRE(parser.has_error());
}

TEST_CASE("HTTP response parser - case insensitive headers", "[http][parser]") {
    response_parser parser;

    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "content-type: application/json\r\n"
        "CONTENT-LENGTH: 2\r\n"
        "\r\n"
        "{}";

    auto [result, consumed] = parser.parse(response);

    REQUIRE(result == parse_result::complete);
    REQUIRE(parser.get_headers().get("Content-Type") == "application/json");
    REQUIRE(parser.get_headers().content_length().value() == 2);
}

TEST_CASE("HTTP response parser - header whitespace trimming", "[http][parser]") {
    response_parser parser;

    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "X-Custom:   value with spaces   \r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    auto [result, consumed] = parser.parse(response);

    REQUIRE(result == parse_result::complete);
    REQUIRE(parser.get_headers().get("X-Custom") == "value with spaces");
}

TEST_CASE("HTTP response parser - reset", "[http][parser]") {
    response_parser parser;

    std::string response1 = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    auto [r1, c1] = parser.parse(response1);
    REQUIRE(r1 == parse_result::complete);
    REQUIRE(parser.status_code() == 404);

    parser.reset();

    std::string response2 = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nfoo";
    auto [r2, c2] = parser.parse(response2);
    REQUIRE(r2 == parse_result::complete);
    REQUIRE(parser.status_code() == 200);
    REQUIRE(parser.body() == "foo");
}

// URL parsing tests

TEST_CASE("URL parsing - basic URLs", "[http][url]") {
    SECTION("Simple HTTP URL") {
        auto u = url::parse("http://example.com/path");
        REQUIRE(u.has_value());
        REQUIRE(u->scheme == "http");
        REQUIRE(u->host == "example.com");
        REQUIRE(u->port == 0);
        REQUIRE(u->path == "/path");
        REQUIRE(u->effective_port() == 80);
        REQUIRE_FALSE(u->is_secure());
    }

    SECTION("HTTPS URL with port") {
        auto u = url::parse("https://example.com:8443/api");
        REQUIRE(u.has_value());
        REQUIRE(u->scheme == "https");
        REQUIRE(u->host == "example.com");
        REQUIRE(u->port == 8443);
        REQUIRE(u->path == "/api");
        REQUIRE(u->effective_port() == 8443);
        REQUIRE(u->is_secure());
    }

    SECTION("URL with query string") {
        auto u = url::parse("http://example.com/search?q=test&page=1");
        REQUIRE(u.has_value());
        REQUIRE(u->path == "/search");
        REQUIRE(u->query == "q=test&page=1");
        REQUIRE(u->path_with_query() == "/search?q=test&page=1");
    }

    SECTION("URL with fragment") {
        auto u = url::parse("http://example.com/page#section");
        REQUIRE(u.has_value());
        REQUIRE(u->path == "/page");
        REQUIRE(u->fragment == "section");
    }

    SECTION("URL with userinfo") {
        auto u = url::parse("http://user:pass@example.com/");
        REQUIRE(u.has_value());
        REQUIRE(u->userinfo == "user:pass");
        REQUIRE(u->host == "example.com");
        REQUIRE(u->authority() == "user:pass@example.com");
        REQUIRE(u->host_authority() == "example.com");
    }
}

TEST_CASE("URL parsing - edge cases", "[http][url]") {
    SECTION("No scheme implies http") {
        auto u = url::parse("example.com/path");
        REQUIRE(u.has_value());
        REQUIRE(u->scheme == "http");
        REQUIRE(u->host == "example.com");
    }

    SECTION("No path implies /") {
        auto u = url::parse("http://example.com");
        REQUIRE(u.has_value());
        REQUIRE(u->path == "/");
    }

    SECTION("Empty host is invalid") {
        auto u = url::parse("http:///path");
        REQUIRE_FALSE(u.has_value());
    }

    SECTION("Mixed case scheme") {
        auto u = url::parse("HTTPS://Example.COM/Path");
        REQUIRE(u.has_value());
        REQUIRE(u->scheme == "https");
        REQUIRE(u->host == "Example.COM");  // Host preserved
    }
}

TEST_CASE("URL parsing - IPv6", "[http][url]") {
    auto u = url::parse("http://[::1]:8080/test");
    REQUIRE(u.has_value());
    REQUIRE(u->host == "::1");
    REQUIRE(u->port == 8080);
    REQUIRE(u->path == "/test");
    REQUIRE(u->authority() == "[::1]:8080");
    REQUIRE(u->host_authority() == "[::1]:8080");
    REQUIRE(u->to_string() == "http://[::1]:8080/test");
}

TEST_CASE("URL header authority excludes userinfo and brackets IPv6", "[http][url]") {
    SECTION("userinfo is preserved in URL authority only") {
        auto u = url::parse("https://user:pass@example.com:8443/resource");
        REQUIRE(u.has_value());
        REQUIRE(u->authority() == "user:pass@example.com:8443");
        REQUIRE(u->host_authority() == "example.com:8443");
    }

    SECTION("IPv6 literals are bracketed in header authority") {
        auto u = url::parse("https://user:pass@[2001:db8::1]:8443/resource");
        REQUIRE(u.has_value());
        REQUIRE(u->host == "2001:db8::1");
        REQUIRE(u->authority() == "user:pass@[2001:db8::1]:8443");
        REQUIRE(u->host_authority() == "[2001:db8::1]:8443");
    }

    SECTION("default ports are omitted consistently") {
        auto u = url::parse("https://[2001:db8::1]:443/resource");
        REQUIRE(u.has_value());
        REQUIRE(u->authority() == "[2001:db8::1]");
        REQUIRE(u->host_authority() == "[2001:db8::1]");
    }
}

TEST_CASE("URL reference resolution follows RFC 3986 path semantics",
          "[http][url]") {
    auto base = url::parse("http://example.com/dir/page.html?old=1#frag");
    REQUIRE(base.has_value());

    SECTION("relative paths merge with the base directory and remove dot segments") {
        auto resolved = url::resolve_reference(*base, "../next?q=1#section");
        REQUIRE(resolved.has_value());
        REQUIRE(resolved->scheme == "http");
        REQUIRE(resolved->host == "example.com");
        REQUIRE(resolved->path == "/next");
        REQUIRE(resolved->query == "q=1");
        REQUIRE(resolved->fragment == "section");
    }

    SECTION("absolute paths replace the base path") {
        auto resolved = url::resolve_reference(*base, "/root/./final?x=1#top");
        REQUIRE(resolved.has_value());
        REQUIRE(resolved->path == "/root/final");
        REQUIRE(resolved->query == "x=1");
        REQUIRE(resolved->fragment == "top");
    }

    SECTION("query-only references keep the base path and replace the query") {
        auto resolved = url::resolve_reference(*base, "?new=1");
        REQUIRE(resolved.has_value());
        REQUIRE(resolved->path == "/dir/page.html");
        REQUIRE(resolved->query == "new=1");
        REQUIRE(resolved->fragment.empty());
    }

    SECTION("fragment-only references keep the request target") {
        auto resolved = url::resolve_reference(*base, "#new");
        REQUIRE(resolved.has_value());
        REQUIRE(resolved->path == "/dir/page.html");
        REQUIRE(resolved->query == "old=1");
        REQUIRE(resolved->fragment == "new");
        REQUIRE(resolved->path_with_query() == "/dir/page.html?old=1");
    }

    SECTION("scheme-relative references inherit the base scheme") {
        auto resolved = url::resolve_reference(*base, "//other.example/a/../b");
        REQUIRE(resolved.has_value());
        REQUIRE(resolved->scheme == "http");
        REQUIRE(resolved->host == "other.example");
        REQUIRE(resolved->path == "/b");
    }

    SECTION("absolute references are accepted case-insensitively") {
        auto resolved = url::resolve_reference(*base, "HTTPS://Example.COM/a/./b");
        REQUIRE(resolved.has_value());
        REQUIRE(resolved->scheme == "https");
        REQUIRE(resolved->host == "Example.COM");
        REQUIRE(resolved->path == "/a/b");
    }
}

TEST_CASE("URL encoding/decoding", "[http][url]") {
    SECTION("Encode special characters") {
        REQUIRE(url_encode("hello world") == "hello%20world");
        REQUIRE(url_encode("foo=bar") == "foo%3Dbar");
        REQUIRE(url_encode("test&value") == "test%26value");
    }

    SECTION("Decode percent-encoded") {
        REQUIRE(url_decode("hello%20world") == "hello world");
        REQUIRE(url_decode("foo%3Dbar") == "foo=bar");
        REQUIRE(url_decode("test%26value") == "test&value");
    }

    SECTION("Plus as space") {
        REQUIRE(url_decode("hello+world") == "hello world");
    }

    SECTION("Unreserved characters unchanged") {
        std::string unreserved = "abcABC123-_.~";
        REQUIRE(url_encode(unreserved) == unreserved);
    }
}

TEST_CASE("Query string parsing", "[http][url]") {
    SECTION("Simple pairs") {
        auto params = parse_query_string("foo=bar&baz=qux");
        REQUIRE(params["foo"] == "bar");
        REQUIRE(params["baz"] == "qux");
    }

    SECTION("URL-encoded values") {
        auto params = parse_query_string("name=John%20Doe&city=New%20York");
        REQUIRE(params["name"] == "John Doe");
        REQUIRE(params["city"] == "New York");
    }

    SECTION("Empty value") {
        auto params = parse_query_string("flag&key=value");
        REQUIRE(params["flag"] == "");
        REQUIRE(params["key"] == "value");
    }
}

// Headers tests

TEST_CASE("Headers collection", "[http][headers]") {
    headers h;

    SECTION("Set and get") {
        h.set("Content-Type", "text/html");
        REQUIRE(h.get("Content-Type") == "text/html");
        REQUIRE(h.get("content-type") == "text/html");  // Case insensitive
        REQUIRE(h.get("CONTENT-TYPE") == "text/html");
    }

    SECTION("Contains") {
        h.set("X-Custom", "value");
        REQUIRE(h.contains("X-Custom"));
        REQUIRE(h.contains("x-custom"));
        REQUIRE_FALSE(h.contains("X-Other"));
    }

    SECTION("Remove") {
        h.set("X-Remove", "value");
        REQUIRE(h.contains("X-Remove"));
        h.remove("x-remove");
        REQUIRE_FALSE(h.contains("X-Remove"));
    }

    SECTION("Content-Length") {
        h.set_content_length(1234);
        REQUIRE(h.content_length().value() == 1234);
    }

    SECTION("Keep-alive detection") {
        headers h1;
        h1.set("Connection", "keep-alive");
        REQUIRE(h1.keep_alive("1.1"));

        headers h2;
        h2.set("Connection", "close");
        REQUIRE_FALSE(h2.keep_alive("1.1"));

        headers h3;
        REQUIRE(h3.keep_alive("1.1"));  // Default for HTTP/1.1
        REQUIRE_FALSE(h3.keep_alive("1.0"));  // Default for HTTP/1.0
    }

    SECTION("Chunked detection") {
        headers h1;
        h1.set("Transfer-Encoding", "chunked");
        REQUIRE(h1.is_chunked());

        headers h2;
        REQUIRE_FALSE(h2.is_chunked());
    }

    SECTION("Repeated list headers keep combined and individual values") {
        h.add("Cache-Control", "no-cache");
        h.add("cache-control", "private");

        REQUIRE(h.get("CACHE-CONTROL") == "no-cache, private");
        auto values = h.get_all("cache-control");
        REQUIRE(values.size() == 2);
        REQUIRE(values[0] == "no-cache");
        REQUIRE(values[1] == "private");
    }

    SECTION("Set-Cookie repeats are not comma joined") {
        h.add("Set-Cookie", "a=1");
        h.add("set-cookie", "b=2");

        REQUIRE(h.get("SET-COOKIE") == "a=1");
        auto values = h.get_all("set-cookie");
        REQUIRE(values.size() == 2);
        REQUIRE(values[0] == "a=1");
        REQUIRE(values[1] == "b=2");

        auto serialized = h.serialize();
        REQUIRE(serialized.find("Set-Cookie: a=1\r\n") != std::string::npos);
        REQUIRE(serialized.find("Set-Cookie: b=2\r\n") != std::string::npos);
        REQUIRE(serialized.find("a=1, b=2") == std::string::npos);
    }

    SECTION("Matching duplicate Content-Length is stored once") {
        h.add("Content-Length", "5");
        h.add("content-length", " 5 ");

        REQUIRE(h.get("CONTENT-LENGTH") == "5");
        auto values = h.get_all("content-length");
        REQUIRE(values.size() == 1);
        REQUIRE(values[0] == "5");
    }
}

TEST_CASE("HTTP response parser preserves duplicate header values",
          "[http][parser][headers]") {
    response_parser parser;
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Set-Cookie: a=1\r\n"
        "Set-Cookie: b=2\r\n"
        "Cache-Control: no-cache\r\n"
        "Cache-Control: private\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    auto [result, consumed] = parser.parse(response);

    REQUIRE(result == parse_result::complete);
    REQUIRE(consumed == response.size());
    auto cookies = parser.get_headers().get_all("set-cookie");
    REQUIRE(cookies.size() == 2);
    REQUIRE(cookies[0] == "a=1");
    REQUIRE(cookies[1] == "b=2");
    REQUIRE(parser.get_headers().get("Set-Cookie") == "a=1");

    auto cache_control = parser.get_headers().get_all("cache-control");
    REQUIRE(cache_control.size() == 2);
    REQUIRE(cache_control[0] == "no-cache");
    REQUIRE(cache_control[1] == "private");
    REQUIRE(parser.get_headers().get("Cache-Control") == "no-cache, private");
}

TEST_CASE("Method to string conversion", "[http][method]") {
    REQUIRE(method_to_string(method::GET) == "GET");
    REQUIRE(method_to_string(method::POST) == "POST");
    REQUIRE(method_to_string(method::PUT) == "PUT");
    REQUIRE(method_to_string(method::DELETE_) == "DELETE");
    REQUIRE(method_to_string(method::PATCH) == "PATCH");
    REQUIRE(method_to_string(method::HEAD) == "HEAD");
    REQUIRE(method_to_string(method::OPTIONS) == "OPTIONS");
}

TEST_CASE("Status reason phrase", "[http][status]") {
    REQUIRE(status_reason(status::ok) == "OK");
    REQUIRE(status_reason(status::not_found) == "Not Found");
    REQUIRE(status_reason(status::internal_server_error) == "Internal Server Error");
    REQUIRE(status_reason(status::moved_permanently) == "Moved Permanently");
    REQUIRE(status_reason(status::unauthorized) == "Unauthorized");
}

// HTTP Message tests

TEST_CASE("HTTP request serialization", "[http][message]") {
    SECTION("Simple GET request") {
        request req(method::GET, "/api/users");
        req.set_host("example.com");

        std::string serialized = req.serialize();

        REQUIRE(serialized.find("GET /api/users HTTP/1.1\r\n") != std::string::npos);
        REQUIRE(serialized.find("Host: example.com\r\n") != std::string::npos);
        REQUIRE(serialized.ends_with("\r\n\r\n"));
    }

    SECTION("POST request with body") {
        request req(method::POST, "/api/data");
        req.set_host("example.com");
        req.set_body(std::string_view("{\"key\":\"value\"}"));
        req.set_content_type("application/json");

        std::string serialized = req.serialize();

        REQUIRE(serialized.find("POST /api/data HTTP/1.1\r\n") != std::string::npos);
        REQUIRE(serialized.find("Content-Type: application/json\r\n") != std::string::npos);
        REQUIRE(serialized.find("{\"key\":\"value\"}") != std::string::npos);
    }

    SECTION("Request with query string") {
        request req(method::GET, "/search");
        req.set_query("q=test&page=1");
        req.set_host("example.com");

        std::string serialized = req.serialize();

        REQUIRE(serialized.find("GET /search?q=test&page=1 HTTP/1.1\r\n") != std::string::npos);
    }
}

// Security regression tests --------------------------------------------------

TEST_CASE("HTTP request parser rejects CL+TE smuggling", "[http][parser][security]") {
    request_parser parser;
    std::string request =
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 5\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n\r\n";

    auto [result, _] = parser.parse(request);
    REQUIRE(result == parse_result::error);
    REQUIRE(parser.has_error());
}

TEST_CASE("HTTP request parser rejects duplicate Content-Length conflict",
          "[http][parser][security]") {
    request_parser parser;
    std::string request =
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 5\r\n"
        "Content-Length: 6\r\n"
        "\r\n"
        "abcde";

    auto [result, _] = parser.parse(request);
    REQUIRE(result == parse_result::error);
}

TEST_CASE("HTTP request parser rejects empty duplicate Content-Length conflict",
          "[http][parser][security]") {
    request_parser parser;
    std::string request =
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length:\r\n"
        "Content-Length: 1\r\n"
        "\r\n"
        "x";

    auto [result, _] = parser.parse(request);
    REQUIRE(result == parse_result::error);
    REQUIRE(parser.has_error());
}

TEST_CASE("HTTP request parser accepts duplicate Content-Length when matching",
          "[http][parser][security]") {
    request_parser parser;
    std::string request =
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 5\r\n"
        "Content-Length:  5  \r\n"  // OWS-equivalent value, allowed
        "\r\n"
        "abcde";

    auto [result, _] = parser.parse(request);
    REQUIRE(result == parse_result::complete);
    REQUIRE(parser.body() == "abcde");
}

TEST_CASE("HTTP request parser rejects unsupported Transfer-Encoding without chunked",
          "[http][parser][security]") {
    request_parser parser;
    std::string request =
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Transfer-Encoding: gzip\r\n"  // no trailing "chunked"
        "\r\n";

    auto [result, _] = parser.parse(request);
    REQUIRE(result == parse_result::error);
}

TEST_CASE("HTTP request parser rejects negative or trailing-garbage Content-Length",
          "[http][parser][security]") {
    SECTION("Negative") {
        request_parser parser;
        auto [r, _] = parser.parse(
            "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n");
        REQUIRE(r == parse_result::error);
    }
    SECTION("Trailing garbage") {
        request_parser parser;
        auto [r, _] = parser.parse(
            "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5x\r\n\r\nhello");
        REQUIRE(r == parse_result::error);
    }
}

TEST_CASE("HTTP chunked detection requires final coding to be chunked",
          "[http][headers][security]") {
    headers h1;
    h1.set("Transfer-Encoding", "x-chunked");  // similar substring, not real chunked
    REQUIRE_FALSE(h1.is_chunked());

    headers h2;
    h2.set("Transfer-Encoding", "gzip, chunked");
    REQUIRE(h2.is_chunked());

    headers h3;
    h3.set("Transfer-Encoding", "chunked, gzip");  // chunked is NOT final
    REQUIRE_FALSE(h3.is_chunked());

    headers h4;
    h4.set("Transfer-Encoding", "  ChUnKeD  ");  // case + OWS
    REQUIRE(h4.is_chunked());
}

TEST_CASE("HTTP headers reject control characters in values",
          "[http][headers][security]") {
    headers h;
    REQUIRE_THROWS_AS(h.set("Location", "/next\r\nX-Evil: yes"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(h.set("Location", "/next\nfoo"), std::invalid_argument);
    REQUIRE_THROWS_AS(h.set("X", std::string("\0", 1)), std::invalid_argument);
    REQUIRE_THROWS_AS(h.set("X", std::string("\x01", 1)), std::invalid_argument);
    // OK to set a clean value
    REQUIRE_NOTHROW(h.set("Location", "/next"));
}

TEST_CASE("HTTP URL parser rejects raw request-splitting bytes",
          "[http][url][security]") {
    REQUIRE_FALSE(url::parse("http://example.com/path\r\nInjected: yes"));
    REQUIRE_FALSE(url::parse("http://example.com/path?x=1\nInjected: yes"));
    REQUIRE_FALSE(url::parse("http://exa\r\nmple.com/"));
    REQUIRE_FALSE(url::parse("http://example.com/raw space"));
    REQUIRE(url::parse("http://example.com/%0d%0a"));
}

TEST_CASE("HTTP URL parser rejects malformed ports",
          "[http][url][security]") {
    REQUIRE_FALSE(url::parse("http://example.com:/"));
    REQUIRE_FALSE(url::parse("http://example.com:80abc/"));
    REQUIRE_FALSE(url::parse("http://example.com:abc/"));
    REQUIRE_FALSE(url::parse("http://example.com:0/"));
    REQUIRE_FALSE(url::parse("http://example.com:65536/"));
    REQUIRE_FALSE(url::parse("http://example.com:80:90/"));
    REQUIRE_FALSE(url::parse("http://::1/"));
    REQUIRE_FALSE(url::parse("http://2001:db8::1/"));

    REQUIRE_FALSE(url::parse("http://[::1]:/"));
    REQUIRE_FALSE(url::parse("http://[::1]:0/"));
    REQUIRE_FALSE(url::parse("http://[::1]:443x/"));
    REQUIRE_FALSE(url::parse("http://[::1]junk/"));
}

TEST_CASE("HTTP request serialization rejects invalid request targets",
          "[http][message][security]") {
    REQUIRE_THROWS_AS(request(method::GET, "/ok\r\nInjected: yes"),
                      std::invalid_argument);

    request req(method::GET, "/ok");
    REQUIRE_THROWS_AS(req.set_query("x=1\nInjected: yes"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(req.set_path("/raw space"), std::invalid_argument);

    REQUIRE_NOTHROW(req.set_query("x=%0d%0a"));
    REQUIRE_NOTHROW(req.serialize());
}

TEST_CASE("HTTP message versions reject invalid serialization bytes",
          "[http][message][security]") {
    request req(method::GET, "/");

    REQUIRE_NOTHROW(req.set_version(""));
    REQUIRE(req.serialize().starts_with("GET / HTTP/1.1\r\n"));

    REQUIRE_NOTHROW(req.set_version("HTTP/2.0"));
    REQUIRE(req.serialize().starts_with("GET / HTTP/2.0\r\n"));

    REQUIRE_THROWS_AS(req.set_version("HTTP/1.1\r\nInjected: yes"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(req.set_version("HTTP/1.1 extra"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(req.set_version("HTTP/one.one"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(req.set_version("HTTP/11.22"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(req.set_version("HTTP/1.10"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(req.set_version("HTTP/10.1"),
                      std::invalid_argument);

    response resp(status::ok);

    REQUIRE_NOTHROW(resp.set_version(""));
    REQUIRE(resp.serialize().starts_with("HTTP/1.1 200 OK\r\n"));

    REQUIRE_NOTHROW(resp.set_version("HTTP/2.0"));
    REQUIRE(resp.serialize().starts_with("HTTP/2.0 200 OK\r\n"));

    REQUIRE_THROWS_AS(resp.set_version("HTTP/1.1\r\nInjected: yes"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(resp.set_version("HTTP/1.1 extra"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(resp.set_version("HTTP/one.one"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(resp.set_version("HTTP/11.22"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(resp.set_version("HTTP/1.10"),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(resp.set_version("HTTP/10.1"),
                      std::invalid_argument);
}

TEST_CASE("HTTP headers reject malformed names", "[http][headers][security]") {
    headers h;
    REQUIRE_THROWS_AS(h.set("", "v"), std::invalid_argument);
    REQUIRE_THROWS_AS(h.set("Bad Name", "v"), std::invalid_argument);  // space in name
    REQUIRE_THROWS_AS(h.set("Bad:Name", "v"), std::invalid_argument);
    REQUIRE_NOTHROW(h.set("X-OK", "v"));
}

TEST_CASE("HTTP chunk size overflow is rejected", "[http][parser][security]") {
    request_parser parser;
    // 17 hex digits of 'f' would overflow a 64-bit size_t (16 hex digits = 64 bits).
    std::string request =
        "POST / HTTP/1.1\r\n"
        "Host: a\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "fffffffffffffffff\r\n"
        "x";

    auto [result, _] = parser.parse(request);
    REQUIRE(result == parse_result::error);
}

TEST_CASE("HTTP chunk size beyond max is rejected", "[http][parser][security]") {
    request_parser parser;
    // 0x7fffffff = 2 GiB > kMaxChunkSize (1 GiB)
    std::string request =
        "POST / HTTP/1.1\r\n"
        "Host: a\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "7fffffff\r\n";

    auto [result, _] = parser.parse(request);
    REQUIRE(result == parse_result::error);
}

TEST_CASE("HTTP request parser validates chunk metadata and trailers",
          "[http][parser][security]") {
    SECTION("accepts valid chunk extensions and trailers") {
        request_parser parser;
        std::string request =
            "POST / HTTP/1.1\r\n"
            "Host: a\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "4;foo=bar; quoted=\"a b\"\r\n"
            "Wiki\r\n"
            "0\r\n"
            "X-Trailer: ok\r\n"
            "\r\n";

        auto [result, _] = parser.parse(request);
        REQUIRE(result == parse_result::complete);
        REQUIRE(parser.body() == "Wiki");
    }

    SECTION("rejects garbage after chunk-size whitespace") {
        request_parser parser;
        std::string request =
            "POST / HTTP/1.1\r\n"
            "Host: a\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "4 garbage\r\n"
            "Wiki\r\n"
            "0\r\n\r\n";

        auto [result, _] = parser.parse(request);
        REQUIRE(result == parse_result::error);
    }

    SECTION("rejects malformed chunk extension syntax") {
        request_parser parser;
        std::string request =
            "POST / HTTP/1.1\r\n"
            "Host: a\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "4; @bad\r\n"
            "Wiki\r\n"
            "0\r\n\r\n";

        auto [result, _] = parser.parse(request);
        REQUIRE(result == parse_result::error);
    }

    SECTION("rejects malformed trailer header syntax") {
        request_parser parser;
        std::string request =
            "POST / HTTP/1.1\r\n"
            "Host: a\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "0\r\n"
            "Bad Trailer: nope\r\n"
            "\r\n";

        auto [result, _] = parser.parse(request);
        REQUIRE(result == parse_result::error);
    }

    SECTION("accepts split chunk extensions, quoted-pairs, and zero-size extensions") {
        request_parser parser;
        std::string first =
            "POST / HTTP/1.1\r\n"
            "Host: a\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "4;foo=\"";

        auto [partial, _1] = parser.parse(first);
        REQUIRE(partial == parse_result::need_more);
        REQUIRE_FALSE(parser.has_error());

        auto [result, _2] = parser.parse(
            "a\\\"b\\\\c\"\r\n"
            "Wiki\r\n"
            "0;done=true\r\n"
            "X-Trailer: ok\r\n"
            "\r\n");
        REQUIRE(result == parse_result::complete);
        REQUIRE(parser.body() == "Wiki");
    }

    SECTION("rejects trailers beyond max header count") {
        request_parser parser;
        parser.set_max_headers(2);
        std::string request =
            "POST / HTTP/1.1\r\n"
            "Host: a\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "0\r\n"
            "X-Trailer: ok\r\n"
            "\r\n";

        auto [result, _] = parser.parse(request);
        REQUIRE(result == parse_result::error);
    }

    SECTION("rejects oversized trailer lines") {
        request_parser parser;
        parser.set_max_header_size(32);
        std::string request =
            "POST / HTTP/1.1\r\n"
            "Host: a\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "0\r\n"
            "X-Trailer: " + std::string(40, 'x') + "\r\n"
            "\r\n";

        auto [result, _] = parser.parse(request);
        REQUIRE(result == parse_result::error);
    }
}

// Mirror of the request-side hardening tests on the response parser. These
// guard against a regression where a hostile *server* — typically reachable
// through a proxy — exploits the same RFC 7230 §3.3.3 ambiguity to smuggle
// a follow-on response into a keep-alive connection.
TEST_CASE("HTTP response parser rejects CL+TE smuggling",
          "[http][parser][security]") {
    response_parser parser;
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n\r\n";

    auto [result, _] = parser.parse(response);
    REQUIRE(result == parse_result::error);
    REQUIRE(parser.has_error());
}

TEST_CASE("HTTP response parser rejects empty duplicate Content-Length conflict",
          "[http][parser][security]") {
    response_parser parser;
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length:\r\n"
        "Content-Length: 1\r\n"
        "\r\n"
        "x";

    auto [result, _] = parser.parse(response);
    REQUIRE(result == parse_result::error);
    REQUIRE(parser.has_error());
}

TEST_CASE("HTTP response parser rejects chunk size overflow",
          "[http][parser][security]") {
    response_parser parser;
    // 17 hex 'f' digits would overflow size_t (16 hex digits = 64 bits).
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "fffffffffffffffff\r\n"
        "x";

    auto [result, _] = parser.parse(response);
    REQUIRE(result == parse_result::error);
}

TEST_CASE("HTTP response parser rejects chunk size beyond max",
          "[http][parser][security]") {
    response_parser parser;
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "7fffffff\r\n";  // 2 GiB > kMaxChunkSize (1 GiB)

    auto [result, _] = parser.parse(response);
    REQUIRE(result == parse_result::error);
}

TEST_CASE("HTTP response parser validates chunk metadata and trailers",
          "[http][parser][security]") {
    SECTION("accepts valid chunk extensions and trailers") {
        response_parser parser;
        std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "4;foo=bar; quoted=\"a b\"\r\n"
            "Wiki\r\n"
            "0\r\n"
            "X-Trailer: ok\r\n"
            "\r\n";

        auto [result, _] = parser.parse(response);
        REQUIRE(result == parse_result::complete);
        REQUIRE(parser.body() == "Wiki");
    }

    SECTION("rejects garbage after chunk-size whitespace") {
        response_parser parser;
        std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "4 garbage\r\n"
            "Wiki\r\n"
            "0\r\n\r\n";

        auto [result, _] = parser.parse(response);
        REQUIRE(result == parse_result::error);
    }

    SECTION("rejects malformed chunk extension syntax") {
        response_parser parser;
        std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "4; @bad\r\n"
            "Wiki\r\n"
            "0\r\n\r\n";

        auto [result, _] = parser.parse(response);
        REQUIRE(result == parse_result::error);
    }

    SECTION("rejects malformed trailer header syntax") {
        response_parser parser;
        std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "0\r\n"
            "Bad Trailer: nope\r\n"
            "\r\n";

        auto [result, _] = parser.parse(response);
        REQUIRE(result == parse_result::error);
    }

    SECTION("accepts split chunk extensions, quoted-pairs, and zero-size extensions") {
        response_parser parser;
        std::string first =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "4;foo=\"";

        auto [partial, _1] = parser.parse(first);
        REQUIRE(partial == parse_result::need_more);
        REQUIRE_FALSE(parser.has_error());

        auto [result, _2] = parser.parse(
            "a\\\"b\\\\c\"\r\n"
            "Wiki\r\n"
            "0;done=true\r\n"
            "X-Trailer: ok\r\n"
            "\r\n");
        REQUIRE(result == parse_result::complete);
        REQUIRE(parser.body() == "Wiki");
    }

    SECTION("rejects trailers beyond max header count") {
        response_parser parser;
        parser.set_max_headers(1);
        std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "0\r\n"
            "X-Trailer: ok\r\n"
            "\r\n";

        auto [result, _] = parser.parse(response);
        REQUIRE(result == parse_result::error);
    }

    SECTION("rejects oversized trailer lines") {
        response_parser parser;
        parser.set_max_header_size(32);
        std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "0\r\n"
            "X-Trailer: " + std::string(40, 'x') + "\r\n"
            "\r\n";

        auto [result, _] = parser.parse(response);
        REQUIRE(result == parse_result::error);
    }
}

TEST_CASE("HTTP response parser bytes_remaining flags pipelined trailers",
          "[http][parser][security]") {
    // After is_complete() is true, any bytes still in the parser's buffer
    // are pipelined data the server pushed past the response. The HTTP
    // client uses bytes_remaining() to refuse to return such a connection
    // to the keep-alive pool (response-splitting prevention).
    response_parser parser;
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello"
        // smuggled bytes left over after the framed response:
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";

    auto [result, _] = parser.parse(response);
    REQUIRE(result == parse_result::complete);
    REQUIRE(parser.is_complete());
    REQUIRE(parser.body() == "hello");
    REQUIRE(parser.bytes_remaining() > 0);
}

TEST_CASE("HTTP response parser bytes_buffered tracks parser footprint",
          "[http][parser][security]") {
    // The HTTP client reads in chunks and after each parse() call inspects
    // bytes_buffered() to enforce client_config::max_response_size. This
    // guards the accessor's contract: it must include both already-extracted
    // body and any bytes still queued in the parser's input buffer.
    response_parser parser;
    auto [r1, _1] = parser.parse(
        "HTTP/1.1 200 OK\r\nContent-Length: 1024\r\n\r\n");
    REQUIRE(r1 == parse_result::need_more);
    REQUIRE(parser.bytes_buffered() == 0);

    std::string body_chunk(512, 'x');
    auto [r2, _2] = parser.parse(body_chunk);
    REQUIRE(r2 == parse_result::need_more);
    REQUIRE(parser.bytes_buffered() == 512);
}

TEST_CASE("HTTP request from parser roundtrip", "[http][message]") {
    std::string original =
        "POST /api/test HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "{\"key\":\"val\"}";

    request_parser parser;
    auto [result, consumed] = parser.parse(original);
    REQUIRE(result == parse_result::complete);

    request req = request::from_parser(parser);
    REQUIRE(req.get_method() == method::POST);
    REQUIRE(req.path() == "/api/test");
    REQUIRE(req.host() == "example.com");
    REQUIRE(req.body() == "{\"key\":\"val\"}");
}

TEST_CASE("HTTP response creation", "[http][message]") {
    SECTION("Simple text response") {
        response resp(status::ok, "Hello, World!", mime::text_plain);

        REQUIRE(resp.get_status() == status::ok);
        REQUIRE(resp.status_code() == 200);
        REQUIRE(resp.body() == "Hello, World!");
        REQUIRE(resp.get_headers().content_type() == "text/plain");
        REQUIRE(resp.get_headers().content_length().value() == 13);
    }

    SECTION("JSON response") {
        response resp(status::ok, "{\"success\":true}", mime::application_json);

        REQUIRE(resp.get_headers().content_type() == "application/json");
    }

    SECTION("Error response") {
        response resp(status::not_found);

        REQUIRE(resp.get_status() == status::not_found);
        REQUIRE(resp.status_code() == 404);
    }

    SECTION("Redirect response") {
        response resp(status::moved_permanently);
        resp.set_header("Location", "https://new-url.example.com/");

        REQUIRE(resp.is_redirect());
        REQUIRE(resp.header("Location") == "https://new-url.example.com/");
    }
}

TEST_CASE("HTTP response serialization", "[http][message]") {
    SECTION("ordinary response includes body") {
        response resp(status::ok, "Hello", mime::text_plain);
        resp.set_header("X-Custom", "value");

        std::string serialized = resp.serialize();

        REQUIRE(serialized.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
        REQUIRE(serialized.find("Content-Type: text/plain\r\n") != std::string::npos);
        REQUIRE(serialized.find("X-Custom: value\r\n") != std::string::npos);
        REQUIRE(serialized.find("\r\n\r\nHello") != std::string::npos);
    }

    SECTION("HEAD response omits body bytes") {
        response resp(status::ok, "head-body", mime::text_plain);

        std::string serialized = resp.serialize(method::HEAD);

        REQUIRE(serialized.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
        REQUIRE(serialized.find("Content-Length: 9\r\n") != std::string::npos);
        REQUIRE(serialized.find("head-body") == std::string::npos);
    }

    SECTION("no-body response status omits body bytes and framing headers") {
        response resp(status::no_content, "forbidden-body", mime::text_plain);
        resp.set_header("Transfer-Encoding", "chunked");

        std::string serialized = resp.serialize();

        REQUIRE(serialized.find("HTTP/1.1 204 No Content\r\n") != std::string::npos);
        REQUIRE(serialized.find("Content-Length: ") == std::string::npos);
        REQUIRE(serialized.find("Transfer-Encoding: ") == std::string::npos);
        REQUIRE(serialized.find("forbidden-body") == std::string::npos);
    }
}

TEST_CASE("HTTP response from parser roundtrip", "[http][message]") {
    std::string body = "<h1>Hello</h1>";
    std::string original =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;

    response_parser parser;
    auto [result, consumed] = parser.parse(original);
    REQUIRE(result == parse_result::complete);

    response resp = response::from_parser(parser);
    REQUIRE(resp.get_status() == status::ok);
    REQUIRE(resp.body() == body);
    REQUIRE(resp.header("Content-Type") == "text/html");
}

TEST_CASE("HTTP request parser rejects too many headers", "[http][parser][security]") {
    // Build a request with more than the default limit of 100 headers.
    std::string request = "GET / HTTP/1.1\r\nHost: a\r\n";
    for (int i = 0; i < 100; ++i) {
        request += "X-H-" + std::to_string(i) + ": v\r\n";
    }
    request += "\r\n";

    request_parser parser;
    auto [result, _] = parser.parse(request);
    REQUIRE(result == parse_result::error);
    REQUIRE(parser.has_error());
}

TEST_CASE("HTTP request parser rejects too many headers with duplicate names",
          "[http][parser][security]") {
    // Repeating one header name keeps headers_.size() == 1, but header_count_
    // increments per line, so the dedicated counter must fire here.
    std::string request = "GET / HTTP/1.1\r\nHost: a\r\n";
    for (int i = 0; i < 100; ++i) {
        request += "X-Dup: v\r\n";
    }
    request += "\r\n";

    request_parser parser;
    auto [result, _] = parser.parse(request);
    REQUIRE(result == parse_result::error);
    REQUIRE(parser.has_error());
}

TEST_CASE("HTTP request parser accepts exactly max_headers headers",
          "[http][parser][security]") {
    // Exactly 100 unique headers (Host + 99 X-H-*) must be accepted.
    std::string request = "GET / HTTP/1.1\r\nHost: a\r\n";
    for (int i = 0; i < 99; ++i) {
        request += "X-H-" + std::to_string(i) + ": v\r\n";
    }
    request += "\r\n";

    request_parser parser;
    auto [result, _] = parser.parse(request);
    REQUIRE(result == parse_result::complete);
}

TEST_CASE("HTTP request parser rejects oversized header line",
          "[http][parser][security]") {
    // A header value of 8193 bytes exceeds the default 8192-byte limit.
    std::string long_value(8193, 'x');
    std::string request =
        "GET / HTTP/1.1\r\nHost: a\r\nX-Big: " + long_value + "\r\n\r\n";

    request_parser parser;
    auto [result, _] = parser.parse(request);
    REQUIRE(result == parse_result::error);
    REQUIRE(parser.has_error());
}

TEST_CASE("HTTP request parser rejects oversized unterminated header line",
          "[http][parser][security]") {
    std::string request =
        "GET / HTTP/1.1\r\nX: " + std::string(8190, 'x');

    request_parser parser;
    auto [result, _] = parser.parse(request);
    REQUIRE(result == parse_result::error);
    REQUIRE(parser.has_error());
}

TEST_CASE("HTTP request parser accepts a boundary header with split CRLF",
          "[http][parser][security]") {
    std::string first_chunk =
        "GET / HTTP/1.1\r\nX: " + std::string(8189, 'x') + "\r";

    request_parser parser;
    auto [partial, _1] = parser.parse(first_chunk);
    REQUIRE(partial == parse_result::need_more);
    REQUIRE_FALSE(parser.has_error());

    auto [complete, _2] = parser.parse("\n\r\n");
    REQUIRE(complete == parse_result::complete);
}

TEST_CASE("HTTP request parser accepts header line exactly at max_header_size",
          "[http][parser][security]") {
    // A header line of exactly 8192 bytes (name + ": " + value) must be
    // accepted. "X: " is 3 bytes, so the value should be 8192 - 3 = 8189.
    std::string value(8189, 'x');
    std::string request =
        "GET / HTTP/1.1\r\nHost: a\r\nX: " + value + "\r\n\r\n";

    request_parser parser;
    auto [result, _] = parser.parse(request);
    REQUIRE(result == parse_result::complete);
}

TEST_CASE("HTTP response parser rejects too many headers with duplicate names",
          "[http][parser][security]") {
    // Same bypass-via-duplicate-name test for the response-side parser.
    std::string response = "HTTP/1.1 200 OK\r\n";
    for (int i = 0; i < 101; ++i) {
        response += "X-Dup: v\r\n";
    }
    response += "\r\n";

    response_parser parser;
    auto [result, _] = parser.parse(response);
    REQUIRE(result == parse_result::error);
    REQUIRE(parser.has_error());
}

TEST_CASE("HTTP response parser rejects oversized header line",
          "[http][parser][security]") {
    std::string long_value(8193, 'x');
    std::string response =
        "HTTP/1.1 200 OK\r\nX-Big: " + long_value + "\r\n\r\n";

    response_parser parser;
    auto [result, _] = parser.parse(response);
    REQUIRE(result == parse_result::error);
    REQUIRE(parser.has_error());
}

TEST_CASE("HTTP response parser rejects oversized unterminated header line",
          "[http][parser][security]") {
    std::string response =
        "HTTP/1.1 200 OK\r\nX: " + std::string(8190, 'x');

    response_parser parser;
    auto [result, _] = parser.parse(response);
    REQUIRE(result == parse_result::error);
    REQUIRE(parser.has_error());
}

TEST_CASE("HTTP response parser accepts a boundary header with split CRLF",
          "[http][parser][security]") {
    std::string first_chunk =
        "HTTP/1.1 204 No Content\r\nX: " + std::string(8189, 'x') + "\r";

    response_parser parser;
    auto [partial, _1] = parser.parse(first_chunk);
    REQUIRE(partial == parse_result::need_more);
    REQUIRE_FALSE(parser.has_error());

    auto [complete, _2] = parser.parse("\n\r\n");
    REQUIRE(complete == parse_result::complete);
}
