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

    SECTION("Invalid HTTP version") {
        request_parser parser;
        auto [result, consumed] = parser.parse("GET / FTP/1.0\r\n\r\n");
        REQUIRE(result == parse_result::error);
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
        std::string response = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n\r\n";
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

    std::string response1 = "HTTP/1.1 404 Not Found\r\n\r\n";
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
    response resp(status::ok, "Hello", mime::text_plain);
    resp.set_header("X-Custom", "value");

    std::string serialized = resp.serialize();

    REQUIRE(serialized.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
    REQUIRE(serialized.find("Content-Type: text/plain\r\n") != std::string::npos);
    REQUIRE(serialized.find("X-Custom: value\r\n") != std::string::npos);
    REQUIRE(serialized.find("\r\n\r\nHello") != std::string::npos);
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
