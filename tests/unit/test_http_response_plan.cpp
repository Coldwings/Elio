#include <catch2/catch_test_macros.hpp>
#include <elio/http/http_response_plan.hpp>
#include <elio/http/http_reply.hpp>

#include <limits>
#include <type_traits>

using namespace elio::http;

TEST_CASE("Public body descriptions distinguish omitted and zero length",
          "[http][response_plan][issue-1210]") {
    SECTION("Omitted streaming length is unknown") {
        const body_description unknown{.kind = response_body_kind::streaming};
        CHECK_FALSE(unknown.length.has_value());
        for (const auto version : {"HTTP/1.1", "HTTP/1.0"}) {
            CAPTURE(version);
            const auto plan = prepare_response(response_head{}, unknown, method::GET, version, true);
            REQUIRE(plan.success());
            CHECK(plan.invoke_producer);
            CHECK_FALSE(plan.expected_body_bytes.has_value());
            CHECK(plan.header_block.find("Content-Length:") == std::string::npos);
            if (std::string_view(version) == "HTTP/1.1") {
                CHECK(plan.framing == response_framing::chunked);
                CHECK(plan.reusable);
                CHECK(plan.header_block.find("Transfer-Encoding: chunked\r\n") != std::string::npos);
            } else {
                CHECK(plan.framing == response_framing::close_delimited);
                CHECK_FALSE(plan.reusable);
                CHECK(plan.header_block.find("Transfer-Encoding:") == std::string::npos);
                CHECK(plan.header_block.find("Connection: close\r\n") != std::string::npos);
            }
        }
    }
    SECTION("Explicit zero describes a known empty body") {
        for (const auto kind : {response_body_kind::complete, response_body_kind::streaming}) {
            CAPTURE(kind);
            const body_description empty{.kind = kind, .length = uint64_t{0}};
            const auto plan = prepare_response(response_head{}, empty, method::GET, "HTTP/1.1", true);
            REQUIRE(plan.success());
            CHECK(plan.framing == response_framing::content_length);
            CHECK(plan.expected_body_bytes == uint64_t{0});
            CHECK(plan.invoke_producer == (kind == response_body_kind::streaming));
            CHECK(plan.header_block.find("Content-Length: 0\r\n") != std::string::npos);
        }
    }
    SECTION("Complete descriptions require an explicit length") {
        const body_description unspecified;
        CHECK(unspecified.kind == response_body_kind::complete);
        CHECK_FALSE(unspecified.length.has_value());
        const auto rejected = prepare_response(
            response_head{}, unspecified, method::GET, "HTTP/1.1", true);
        CHECK_FALSE(rejected.success());
        CHECK(rejected.error == EINVAL);
        CHECK(rejected.header_block.empty());
    }
}

TEST_CASE("Response body setters leave framing assertions to preparation",
          "[http][response][plan][issue-1195]") {
    auto message = response::ok("one");
    REQUIRE_FALSE(message.get_headers().contains("Content-Length"));
    message.set_body(std::string("four"));
    REQUIRE_FALSE(message.get_headers().contains("Content-Length"));
    auto plan = prepare_response(message,
        {response_body_kind::complete, message.body().size()}, method::GET, "HTTP/1.1", true);
    REQUIRE(plan.success());
    REQUIRE(plan.header_block.find("Content-Length: 4\r\n") != std::string::npos);
    REQUIRE_FALSE(message.get_headers().contains("Content-Length"));
    message.set_header("Content-Length", "4");
    message.set_body(std::string_view("longer"));
    REQUIRE(message.header("Content-Length") == "4");
    plan = prepare_response(message,
        {response_body_kind::complete, message.body().size()}, method::GET, "HTTP/1.1", true);
    REQUIRE_FALSE(plan.success());
    REQUIRE(plan.header_block.empty());
}

static_assert(!std::is_copy_constructible_v<streaming_response>);
static_assert(std::is_move_constructible_v<streaming_response>);
static_assert(!std::is_copy_constructible_v<reply>);

namespace {
body_description complete(uint64_t length) {
    return {response_body_kind::complete, length, response_transfer::automatic};
}
body_description streaming(std::optional<uint64_t> length = std::nullopt) {
    return {response_body_kind::streaming, length, response_transfer::automatic};
}
void require_invalid(const response_plan& plan) {
    REQUIRE_FALSE(plan.success());
    REQUIRE(plan.error == EINVAL);
    REQUIRE(plan.header_block.empty());
    REQUIRE_FALSE(plan.invoke_producer);
    REQUIRE_FALSE(plan.reusable);
}
bool has_header(const response_plan& plan, std::string_view line) {
    return plan.header_block.find(std::string("\r\n") + std::string(line) + "\r\n") != std::string::npos;
}
}

TEST_CASE("response head owns only validated metadata", "[http][response_plan]") {
    response_head head(status::created);
    REQUIRE(head.status_code() == 201);
    REQUIRE(head.is_success());
    REQUIRE_FALSE(head.representation_length());
    head.set_representation_length(std::numeric_limits<uint64_t>::max());
    REQUIRE(head.representation_length() == std::numeric_limits<uint64_t>::max());
    head.set_representation_length(std::nullopt);
    head.set_content_type("application/json");
    REQUIRE(head.content_type() == "application/json");
    REQUIRE_THROWS_AS(head.set_header("X-Test", "ok\r\ninjected: value"), std::invalid_argument);
    REQUIRE_THROWS_AS(head.set_header("bad\nname", "value"), std::invalid_argument);
    REQUIRE_THROWS_AS(head.set_version("HTTP/1.1\r\n"), std::invalid_argument);
}

TEST_CASE("complete response copies and moves preserve framing assertions", "[http][response_plan]") {
    response original(status::created, "body");
    original.set_header("Content-Length", "4");
    original.set_header("X-Owner", "original");
    original.set_representation_length(42);
    const auto expected = prepare_response(original, complete(4), method::GET, "HTTP/1.1", true);
    REQUIRE(expected.success());

    const auto verify = [&](const response& value) {
        REQUIRE(value.body() == "body");
        REQUIRE(value.status_code() == 201);
        REQUIRE(value.header("Content-Length") == "4");
        REQUIRE(value.header("X-Owner") == "original");
        REQUIRE(value.representation_length() == 42);
        const auto plan = prepare_response(value, complete(value.body().size()), method::GET, "HTTP/1.1", true);
        REQUIRE(plan.success());
        REQUIRE(plan.header_block == expected.header_block);
        REQUIRE(plan.framing == expected.framing);
        REQUIRE(plan.expected_body_bytes == expected.expected_body_bytes);
        REQUIRE(plan.invoke_producer == expected.invoke_producer);
        REQUIRE(plan.reusable == expected.reusable);
        // GET asserts transferred length; HEAD instead asserts representation
        // length, so retaining both metadata values must retain this conflict.
        require_invalid(prepare_response(value, complete(4), method::HEAD, "HTTP/1.1", true));
        auto normalized = value;
        normalized.get_headers().remove("Content-Length");
        const auto head = prepare_response(normalized, complete(4), method::HEAD, "HTTP/1.1", true);
        REQUIRE(head.success());
        REQUIRE(has_header(head, "Content-Length: 42"));
        REQUIRE(head.framing == response_framing::none);
        REQUIRE_FALSE(head.invoke_producer);
    };

    response copied(original);
    response assigned = response::ok("obsolete");
    assigned.set_header("Content-Length", "8");
    assigned.set_representation_length(99);
    assigned = original;
    original.set_body(std::string_view("changed"));
    original.set_header("X-Owner", "changed");
    original.set_representation_length(99);
    require_invalid(prepare_response(original, complete(original.body().size()), method::GET, "HTTP/1.1", true));
    verify(copied);
    verify(assigned);

    response moved(std::move(copied));
    response move_assigned = response::ok("obsolete");
    move_assigned.set_header("Content-Length", "8");
    move_assigned.set_representation_length(99);
    move_assigned = std::move(assigned);
    verify(moved);
    verify(move_assigned);
}

TEST_CASE("reply owns a move-only producer and shares metadata representation",
          "[http][response_plan]") {
    response ordinary = response::ok("body");
    response_head& metadata = ordinary;
    metadata.set_status(status::created);
    metadata.set_representation_length(42);
    REQUIRE(ordinary.status_code() == 201);
    REQUIRE(ordinary.representation_length() == 42);
    REQUIRE(ordinary.body() == "body");

    struct owned_resource {
        int* destroyed;
        ~owned_resource() { ++*destroyed; }
    };
    int destroyed = 0;
    int calls = 0;
    {
        auto producer = [resource = std::make_unique<owned_resource>(&destroyed), &calls](
            body_writer&, elio::coro::cancel_token) -> elio::coro::task<send_result> {
            (void)resource;
            ++calls;
            co_return send_result{};
        };
        streaming_response stream(status::ok, std::move(producer), 0);
        stream.set_header("X-Stream", "yes");
        reply selected(std::move(stream));
        reply moved(std::move(selected));
        const auto& value = std::get<streaming_response>(moved);
        REQUIRE(value.body_length() == 0);
        REQUIRE(value.header("X-Stream") == "yes");
        REQUIRE(destroyed == 0);
        REQUIRE(calls == 0);
    }
    REQUIRE(destroyed == 1);
    REQUIRE(calls == 0);
}

TEST_CASE("response preflight selects body framing and negotiated version", "[http][response_plan]") {
    struct scenario {
        body_description body;
        std::string_view request_version;
        response_framing framing;
        bool invoke;
        bool reusable;
        std::string_view wire_header;
    };
    const scenario cases[] = {
        {complete(0), "HTTP/1.1", response_framing::content_length, false, true, "Content-Length: 0"},
        {complete(123), "HTTP/1.1", response_framing::content_length, false, true, "Content-Length: 123"},
        {streaming(0), "HTTP/1.1", response_framing::content_length, true, true, "Content-Length: 0"},
        {streaming(123), "HTTP/1.1", response_framing::content_length, true, true, "Content-Length: 123"},
        {streaming(), "HTTP/1.1", response_framing::chunked, true, true, "Transfer-Encoding: chunked"},
        {streaming(), "HTTP/1.0", response_framing::close_delimited, true, false, "Connection: close"},
        {complete(0), "HTTP/1.0", response_framing::content_length, false, true, "Connection: keep-alive"},
        {{response_body_kind::streaming, std::nullopt, response_transfer::close_delimited},
         "HTTP/1.1", response_framing::close_delimited, true, false, "Connection: close"},
    };
    for (const auto& item : cases) {
        CAPTURE(item.request_version, item.body.length, item.framing);
        const auto plan = prepare_response(response_head{}, item.body, method::GET, item.request_version, true);
        REQUIRE(plan.success());
        REQUIRE(plan.framing == item.framing);
        REQUIRE(plan.expected_body_bytes == item.body.length);
        REQUIRE(plan.invoke_producer == item.invoke);
        REQUIRE(plan.reusable == item.reusable);
        REQUIRE(plan.header_block.starts_with(std::string(item.request_version) + " 200 OK\r\n"));
        REQUIRE(plan.header_block.ends_with("\r\n\r\n"));
        REQUIRE(has_header(plan, item.wire_header));
    }
}

TEST_CASE("response preflight validates declared lengths before emitting headers", "[http][response_plan]") {
    for (const std::string_view value : {"", "-1", "+1", "1x", "1, 1", "1, 2", "18446744073709551616"}) {
        CAPTURE(value);
        response_head head;
        head.set_header("Content-Length", value);
        require_invalid(prepare_response(head, complete(1), method::GET, "HTTP/1.1", true));
    }
    response_head head;
    head.set_header("Content-Length", " 001\t");
    REQUIRE(prepare_response(head, complete(1), method::GET, "HTTP/1.1", true).success());
    require_invalid(prepare_response(head, complete(2), method::GET, "HTTP/1.1", true));
    require_invalid(prepare_response(head, streaming(), method::GET, "HTTP/1.1", true));
    head.get_headers().remove("Content-Length");
    head.get_headers().add("Content-Length", "1");
    // Existing headers canonicalizes equivalent field lines before preflight.
    head.get_headers().add("content-length", "1");
    REQUIRE(head.get_headers().get_all("Content-Length").size() == 1);
    REQUIRE(prepare_response(head, complete(1), method::GET, "HTTP/1.1", true).success());
    REQUIRE_THROWS_AS(head.get_headers().add("Content-Length", "2"), std::invalid_argument);
    head.set_header("Content-Length", "18446744073709551615");
    REQUIRE(prepare_response(head, streaming(std::numeric_limits<uint64_t>::max()),
                             method::GET, "HTTP/1.1", true).success());
    require_invalid(prepare_response(response_head{}, {response_body_kind::complete, std::nullopt},
                                     method::GET, "HTTP/1.1", true));
    require_invalid(prepare_response(response_head{}, {response_body_kind::complete, std::nullopt},
                                     method::HEAD, "HTTP/1.1", true));
    require_invalid(prepare_response(response_head{}, {response_body_kind::streaming, 0,
                                     response_transfer::close_delimited}, method::GET, "HTTP/1.1", true));
    require_invalid(prepare_response(head, {response_body_kind::streaming, std::nullopt,
                                     response_transfer::close_delimited}, method::GET, "HTTP/1.1", true));
}

TEST_CASE("HEAD selects metadata without invoking the producer", "[http][response_plan]") {
    for (const auto length : {std::optional<uint64_t>{}, std::optional<uint64_t>{0}, std::optional<uint64_t>{42}}) {
        response_head head;
        auto plan = prepare_response(head, streaming(length), method::HEAD, "HTTP/1.1", true);
        REQUIRE(plan.success());
        REQUIRE(plan.framing == response_framing::none);
        REQUIRE(plan.expected_body_bytes == 0);
        REQUIRE_FALSE(plan.invoke_producer);
        REQUIRE(plan.reusable);
        REQUIRE((plan.header_block.find("Content-Length:") != std::string::npos) == length.has_value());
        head.set_representation_length(99);
        plan = prepare_response(head, streaming(length), method::HEAD, "HTTP/1.1", true);
        REQUIRE(has_header(plan, "Content-Length: 99"));
        head.set_header("Content-Length", "99");
        REQUIRE(prepare_response(head, streaming(length), method::HEAD, "HTTP/1.1", true).success());
        head.set_header("Content-Length", "98");
        require_invalid(prepare_response(head, streaming(length), method::HEAD, "HTTP/1.1", true));
    }
    response_head head;
    head.set_header("Content-Length", "42");
    require_invalid(prepare_response(head, streaming(), method::HEAD, "HTTP/1.1", true));
}

TEST_CASE("bodyless statuses override producer framing but validate metadata", "[http][response_plan]") {
    for (const auto code : {status::no_content, status::reset_content, status::not_modified}) {
        for (const auto request_method : {method::GET, method::HEAD}) {
            response_head head(code);
            const body_description description{response_body_kind::streaming, 123,
                                               response_transfer::close_delimited};
            auto plan = prepare_response(head, description, request_method, "HTTP/1.1", true);
            REQUIRE(plan.success());
            REQUIRE(plan.framing == response_framing::none);
            REQUIRE(plan.expected_body_bytes == 0);
            REQUIRE_FALSE(plan.invoke_producer);
            REQUIRE(plan.reusable);
            REQUIRE((plan.header_block.find("Content-Length:") != std::string::npos) ==
                    (code == status::reset_content));
            head.set_representation_length(42);
            plan = prepare_response(head, description, request_method, "HTTP/1.1", true);
            if (code == status::not_modified) REQUIRE(has_header(plan, "Content-Length: 42"));
            if (code == status::reset_content) REQUIRE(has_header(plan, "Content-Length: 0"));
            head.set_header("Transfer-Encoding", "chunked");
            require_invalid(prepare_response(head, description, request_method, "HTTP/1.1", true));
            head.get_headers().remove("Transfer-Encoding");
            head.set_header("Content-Length", "bad");
            require_invalid(prepare_response(head, description, request_method, "HTTP/1.1", true));
        }
    }
    response_head head(status::not_modified);
    head.set_header("Content-Length", "42");
    REQUIRE(has_header(prepare_response(head, complete(100), method::GET, "HTTP/1.1", true), "Content-Length: 42"));
    head.set_representation_length(43);
    require_invalid(prepare_response(head, complete(100), method::GET, "HTTP/1.1", true));
    head.set_status(status::no_content);
    REQUIRE_FALSE(has_header(prepare_response(head, complete(100), method::GET, "HTTP/1.1", true), "Content-Length: 42"));
    head.set_status(status::reset_content);
    require_invalid(prepare_response(head, complete(100), method::GET, "HTTP/1.1", true));
    head.set_header("Content-Length", "0");
    REQUIRE(has_header(prepare_response(head, complete(100), method::GET, "HTTP/1.1", true), "Content-Length: 0"));
}

TEST_CASE("response preflight rejects unsupported handoff and explicit encodings", "[http][response_plan]") {
    for (const auto code : {100, 101, 103, 199, 600}) {
        require_invalid(prepare_response(response_head(static_cast<status>(code)), complete(0), method::GET, "HTTP/1.1", true));
    }
    for (const auto code : {200, 204, 299}) {
        require_invalid(prepare_response(response_head(static_cast<status>(code)), complete(0), method::CONNECT, "HTTP/1.1", true));
    }
    REQUIRE(prepare_response(response_head(status::bad_request), complete(0), method::CONNECT, "HTTP/1.1", true).success());
    for (const auto encoding : {"", "chunked", "gzip", "gzip, chunked"}) {
        response_head head;
        head.set_header("Transfer-Encoding", encoding);
        require_invalid(prepare_response(head, streaming(), method::GET, "HTTP/1.1", true));
        require_invalid(prepare_response(head, streaming(), method::HEAD, "HTTP/1.1", true));
    }
    response_head head;
    head.set_version("HTTP/2.0");
    require_invalid(prepare_response(head, complete(0), method::GET, "HTTP/1.1", true));
    head.set_version("");
    REQUIRE(prepare_response(head, complete(0), method::GET, "HTTP/1.0", true).header_block.starts_with("HTTP/1.0"));
    require_invalid(prepare_response(head, complete(0), method::GET, "HTTP/2.0", true));
    head.set_version("HTTP/1.0");
    REQUIRE(prepare_response(head, streaming(), method::GET, "HTTP/1.1", true).framing == response_framing::close_delimited);
}

TEST_CASE("connection close wins over keep alive and preflight does not mutate input", "[http][response_plan]") {
    response_head head;
    head.set_header("Connection", "keep-alive, CLOSE");
    head.get_headers().add("Set-Cookie", "a=1");
    head.get_headers().add("Set-Cookie", "b=2");
    auto plan = prepare_response(head, complete(1), method::GET, "HTTP/1.1", true);
    REQUIRE_FALSE(plan.reusable);
    REQUIRE(has_header(plan, "Connection: close"));
    REQUIRE(has_header(plan, "Set-Cookie: a=1"));
    REQUIRE(has_header(plan, "Set-Cookie: b=2"));
    REQUIRE(head.header("Connection") == "keep-alive, CLOSE");
    REQUIRE_FALSE(head.get_headers().contains("Content-Length"));
    head.set_header("Connection", "keep-alive");
    plan = prepare_response(head, complete(1), method::GET, "HTTP/1.1", false);
    REQUIRE_FALSE(plan.reusable);
    REQUIRE(has_header(plan, "Connection: close"));
}

TEST_CASE("complete response serialization uses ordinary final preflight", "[http][response_plan]") {
    for (const auto code : {status::ok, status::no_content, status::reset_content, status::not_modified}) {
        for (const auto verb : {method::GET, method::HEAD}) {
            response resp(code, "body");
            const auto plan = prepare_response(resp, complete(4), verb, "HTTP/1.1", true);
            REQUIRE(plan.success());
            const auto expected = plan.header_block +
                (plan.framing == response_framing::none ? "" : "body");
            REQUIRE(resp.serialize(verb) == expected);
            REQUIRE_FALSE(resp.get_headers().contains("Content-Length"));
            resp.set_header("Transfer-Encoding", "chunked");
            REQUIRE_THROWS_AS(resp.serialize(verb), std::invalid_argument);
        }
    }
    response resp(status::ok, "body");
    for (const auto length : {"3", "5", "4, 4", "bad", ""}) {
        resp.set_header("Content-Length", length);
        REQUIRE_THROWS_AS(resp.serialize(), std::invalid_argument);
    }
    resp.set_header("Content-Length", "4");
    REQUIRE(resp.serialize().ends_with("\r\n\r\nbody"));
    resp.set_version("HTTP/2.0");
    REQUIRE_THROWS_AS(resp.serialize(), std::invalid_argument);
    resp.set_version("HTTP/1.0");
    REQUIRE(resp.serialize().starts_with("HTTP/1.0 200 OK\r\n"));
    REQUIRE(resp.serialize().find("Connection: close\r\n") != std::string::npos);
}

TEST_CASE("serialized HEAD and 304 use explicit representation metadata", "[http][response_plan]") {
    response resp(status::ok, "body");
    resp.set_representation_length(99);
    REQUIRE(resp.serialize(method::HEAD).find("Content-Length: 99\r\n") != std::string::npos);
    REQUIRE(resp.serialize(method::HEAD).ends_with("\r\n\r\n"));
    resp.set_status(status::not_modified);
    REQUIRE(resp.serialize().find("Content-Length: 99\r\n") != std::string::npos);
    resp.set_representation_length(std::nullopt);
    REQUIRE(resp.serialize().find("Content-Length:") == std::string::npos);
    resp.set_header("Content-Length", "42");
    REQUIRE(resp.serialize().find("Content-Length: 42\r\n") != std::string::npos);
    resp.set_status(status::no_content);
    REQUIRE(resp.serialize().find("Content-Length:") == std::string::npos);
    resp.set_status(status::reset_content);
    REQUIRE_THROWS_AS(resp.serialize(), std::invalid_argument);
    resp.set_header("Content-Length", "0");
    REQUIRE(resp.serialize().find("Content-Length: 0\r\n") != std::string::npos);
}

TEST_CASE("protocol header serialization stays separate from ordinary final framing", "[http][response_plan]") {
    for (const auto code : {status::continue_, status::switching_protocols, status::early_hints, status::ok}) {
        response resp(code, "must not be sent");
        resp.set_header("Content-Length", "bad");
        resp.set_header("Transfer-Encoding", "chunked");
        resp.set_header("Upgrade", "websocket");
        const auto verb = code == status::ok ? method::CONNECT : method::GET;
        const auto wire = resp.serialize(verb);
        REQUIRE(wire.find("Content-Length:") == std::string::npos);
        REQUIRE(wire.find("Transfer-Encoding:") == std::string::npos);
        REQUIRE(wire.find("must not be sent") == std::string::npos);
        REQUIRE(wire.find("Upgrade: websocket\r\n") != std::string::npos);
        REQUIRE(wire.ends_with("\r\n\r\n"));
        REQUIRE(resp.header("Content-Length") == "bad");
        require_invalid(prepare_response(resp, complete(resp.body().size()), verb, "HTTP/1.1", true));
        resp.set_version("HTTP/2.0");
        REQUIRE_THROWS_AS(resp.serialize(verb), std::invalid_argument);
    }
}

TEST_CASE("received chunked response needs explicit normalization before reserialization", "[http][response_plan]") {
    const std::string wire = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
    response_parser parser;
    REQUIRE(parser.parse(wire).first == parse_result::complete);
    auto accumulated = response::from_parser(parser);
    REQUIRE(accumulated.body() == "abc");
    REQUIRE(accumulated.header("Transfer-Encoding") == "chunked");
    REQUIRE_THROWS_AS(accumulated.serialize(), std::invalid_argument);
    accumulated.get_headers().remove("Transfer-Encoding");
    REQUIRE(accumulated.serialize().find("Content-Length: 3\r\n") != std::string::npos);
    REQUIRE(accumulated.serialize().ends_with("\r\n\r\nabc"));

    response_decoder decoder;
    std::string_view pending = wire;
    std::string body;
    for (;;) {
        const auto decoded = decoder.decode(pending);
        pending.remove_prefix(decoded.consumed);
        body.append(decoded.body);
        if (decoded.event == response_event::message_complete) break;
        REQUIRE(decoded.event != response_event::error);
        REQUIRE(decoded.event != response_event::need_more);
    }
    auto incremental = response::from_decoder(decoder, std::move(body));
    REQUIRE(incremental.header("Transfer-Encoding") == "chunked");
    REQUIRE_THROWS_AS(incremental.serialize(), std::invalid_argument);
    incremental.get_headers().remove("Transfer-Encoding");
    REQUIRE(incremental.serialize() == accumulated.serialize());
    REQUIRE(decoder.get_headers().get("Transfer-Encoding") == "chunked");
}
