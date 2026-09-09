#include <catch2/catch_test_macros.hpp>
#include <elio/http/http_server.hpp>
#include <elio/http/websocket_server.hpp>
#include <cstring>

namespace {
using namespace elio;
using namespace elio::http;

struct dispatch_stream {
    std::string wire;
    size_t fail_after = SIZE_MAX;
    coro::task<io::io_result> writev(iovec* parts, size_t count, coro::cancel_token token) {
        if (token.is_cancelled()) co_return io::io_result{-ECANCELED, 0};
        if (wire.size() >= fail_after) co_return io::io_result{-EPIPE, 0};
        size_t sent = 0;
        for (size_t i = 0; i < count && sent < 3; ++i) {
            const auto amount = std::min({parts[i].iov_len, size_t{3} - sent, fail_after - wire.size()});
            wire.append(static_cast<const char*>(parts[i].iov_base), amount);
            sent += amount;
        }
        co_return io::io_result{static_cast<int32_t>(sent), 0};
    }
    coro::task<io::io_result> write(const void* data, size_t size, coro::cancel_token token) {
        iovec part{const_cast<void*>(data), size};
        co_return co_await writev(&part, 1, token);
    }
    coro::task<io::io_result> read(void*, size_t, coro::cancel_token) {
        co_return io::io_result{0, 0};
    }
    coro::task<net::write_finish_result> finish_write(coro::cancel_token = {},
        std::chrono::milliseconds = std::chrono::milliseconds(5000)) {
        co_return net::write_finish_result{net::close_scope::write_direction, 0, true, false};
    }
    net::close_scope read_end_scope() const noexcept { return net::close_scope::write_direction; }
};

template<class T>
T dispatch_inline(coro::task<T> task) {
    const auto handle = coro::detail::task_access::handle(task);
    handle.resume();
    REQUIRE(handle.done());
    return task.await_resume();
}

request_parser connect_parser(std::string_view version = "HTTP/1.1", std::string prefix = {}) {
    request_parser parser;
    REQUIRE(parser.parse("CONNECT [::1]:00443 " + std::string(version) +
        "\r\nHost: different.example\r\n\r\n" + prefix).first == parse_result::complete);
    return parser;
}
}

TEST_CASE("CONNECT dispatch sends headers before exact-once binary handoff", "[http][tunnel][dispatch]") {
    for (const auto version : {"HTTP/1.0", "HTTP/1.1"}) {
        const std::string prefix("\0\xffGET /opaque", 13);
        auto parser = connect_parser(version, prefix);
        dispatch_stream stream;
        int calls = 0;
        bool headers_complete = false;
        std::string received;
        reply selected{tunnel_response([&, owned = std::make_unique<int>(7)](
            tunnel_stream& tunnel, coro::cancel_token token) -> coro::task<tunnel_result> {
            ++calls;
            headers_complete = stream.wire.ends_with("\r\n\r\n") && *owned == 7;
            char bytes[32]{};
            const auto first = co_await tunnel.read(bytes, sizeof(bytes), token);
            if (first.result > 0) received.assign(bytes, static_cast<size_t>(first.result));
            const auto second = co_await tunnel.read(bytes, sizeof(bytes), token);
            if (second.result != 0) co_return tunnel_result{tunnel_end::callback_error, EIO};
            co_return tunnel_result{};
        })};
        const auto result = dispatch_inline(http::detail::dispatch_reply(stream, selected, parser, true));
        CHECK(result.success());
        CHECK_FALSE(result.reusable);
        CHECK(calls == 1);
        CHECK(headers_complete);
        CHECK(received == prefix);
        CHECK(parser.take_remaining().empty());
        CHECK(stream.wire.starts_with(std::string(version) + " 200 "));
        CHECK(stream.wire.find("Content-Length:") == std::string::npos);
        CHECK(stream.wire.find("Transfer-Encoding:") == std::string::npos);
        const auto original_wire = stream.wire;
        CHECK_FALSE(dispatch_inline(http::detail::dispatch_reply(stream, selected, parser, true)).success());
        CHECK(stream.wire == original_wire);
        CHECK(calls == 1);
    }
}

TEST_CASE("CONNECT dispatch rejects invalid acceptance without output", "[http][tunnel][dispatch]") {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        CAPTURE(scenario);
        auto parser = connect_parser();
        if (scenario == 0) {
            parser = request_parser{};
            REQUIRE(parser.parse("GET / HTTP/1.1\r\nHost: test\r\n\r\n").first == parse_result::complete);
        }
        int calls = 0;
        tunnel_response response([&](tunnel_stream&, coro::cancel_token) -> coro::task<tunnel_result> {
            ++calls;
            co_return tunnel_result{};
        });
        if (scenario == 1) response.set_status(status::bad_request);
        if (scenario == 2) response.set_header("Content-Length", "0");
        if (scenario == 3) response.set_header("Transfer-Encoding", "chunked");
        if (scenario == 4) response.set_version("HTTP/2.0");
        reply selected{std::move(response)};
        dispatch_stream stream;
        CHECK_FALSE(dispatch_inline(http::detail::dispatch_reply(stream, selected, parser, true)).success());
        CHECK(stream.wire.empty());
        CHECK(calls == 0);
    }
}

TEST_CASE("CONNECT partial acceptance failure retains prefix and skips callback", "[http][tunnel][dispatch]") {
    auto parser = connect_parser("HTTP/1.1", "prefix");
    dispatch_stream stream;
    stream.fail_after = 5;
    int calls = 0;
    reply selected{tunnel_response([&](tunnel_stream&, coro::cancel_token) -> coro::task<tunnel_result> {
        ++calls;
        co_return tunnel_result{};
    })};
    const auto result = dispatch_inline(http::detail::dispatch_reply(stream, selected, parser, true));
    CHECK_FALSE(result.success());
    CHECK_FALSE(result.reusable);
    CHECK(stream.wire.size() == 5);
    CHECK(calls == 0);
    CHECK(parser.take_remaining() == "prefix");
}

TEST_CASE("CONNECT rejection never reuses speculative input", "[http][tunnel][dispatch]") {
    auto parser = connect_parser("HTTP/1.1", "opaque");
    reply selected{response::bad_request("denied")};
    dispatch_stream stream;
    const auto result = dispatch_inline(http::detail::dispatch_reply(stream, selected, parser, true));
    CHECK(result.success());
    CHECK_FALSE(result.reusable);
    CHECK(stream.wire.find("Connection: close\r\n") != std::string::npos);
    CHECK(parser.take_remaining() == "opaque");
}

TEST_CASE("Dedicated CONNECT router passes validated raw authority", "[http][router][tunnel]") {
    websocket::ws_router routes;
    std::string raw, host;
    uint16_t port = 0;
    routes.connect([&](context&, connect_authority_view authority) {
        raw = authority.raw;
        host = authority.host;
        port = authority.port;
        return response::bad_request("policy denied");
    });
    auto parser = connect_parser();
    context ctx(request::from_parser(parser), "test");
    auto response = dispatch_inline(routes.connect_handler()(ctx));
    CHECK(std::holds_alternative<http::response>(response));
    CHECK(raw == "[::1]:00443");
    CHECK(host == "::1");
    CHECK(port == 443);
}

namespace {
template<class Result>
Result make_connect_reply() {
    if constexpr (std::same_as<Result, response>) {
        return response::bad_request("denied");
    } else if constexpr (std::same_as<Result, streaming_response>) {
        return streaming_response(status::bad_request,
            [](body_writer&, coro::cancel_token) -> coro::task<send_result> { co_return send_result{}; }, 0);
    } else if constexpr (std::same_as<Result, tunnel_response>) {
        return tunnel_response([](tunnel_stream&, coro::cancel_token) -> coro::task<tunnel_result> {
            co_return tunnel_result{};
        });
    } else {
        return reply{make_connect_reply<tunnel_response>()};
    }
}

template<class Result>
void check_connect_adapters() {
    router routes;
    auto parser = connect_parser();
    context ctx(request::from_parser(parser), "test");
    constexpr size_t index = std::same_as<Result, response> ? 0 :
        (std::same_as<Result, streaming_response> ? 1 : 2);
    routes.connect([](context&, connect_authority_view) { return make_connect_reply<Result>(); });
    CHECK(dispatch_inline(routes.connect_handler()(ctx)).index() == index);
    routes.connect([](context&, connect_authority_view) -> coro::task<Result> {
        co_return make_connect_reply<Result>();
    });
    CHECK(dispatch_inline(routes.connect_handler()(ctx)).index() == index);
    auto ordinary = http::detail::adapt_handler([](context&) { return make_connect_reply<Result>(); });
    CHECK(dispatch_inline(ordinary(ctx)).index() == index);
    auto async_ordinary = http::detail::adapt_handler([](context&) -> coro::task<Result> {
        co_return make_connect_reply<Result>();
    });
    CHECK(dispatch_inline(async_ordinary(ctx)).index() == index);
}
}

TEST_CASE("CONNECT and ordinary adapters accept all reply forms", "[http][router][tunnel]") {
    check_connect_adapters<response>();
    check_connect_adapters<streaming_response>();
    check_connect_adapters<tunnel_response>();
    check_connect_adapters<reply>();
}

TEST_CASE("CONNECT pre-cancel prevents acceptance and callback exceptions end HTTP", "[http][tunnel][dispatch]") {
    for (const bool cancel : {false, true}) {
        auto parser = connect_parser("HTTP/1.1", "prefix");
        dispatch_stream stream;
        coro::cancel_source source;
        if (cancel) source.cancel();
        int calls = 0;
        reply selected{tunnel_response([&](tunnel_stream&, coro::cancel_token) -> coro::task<tunnel_result> {
            ++calls;
            throw std::runtime_error("session failed");
            co_return tunnel_result{};
        })};
        const auto result = dispatch_inline(http::detail::dispatch_reply(
            stream, selected, parser, true, source.get_token()));
        CHECK_FALSE(result.success());
        CHECK_FALSE(result.reusable);
        if (cancel) {
            CHECK(calls == 0);
            CHECK(stream.wire.empty());
            CHECK(parser.take_remaining() == "prefix");
        } else {
            REQUIRE(result.tunnel.has_value());
            CHECK(result.tunnel->end == tunnel_end::callback_error);
            CHECK(calls == 1);
            CHECK(stream.wire.ends_with("\r\n\r\n"));
            CHECK(stream.wire.find("500") == std::string::npos);
        }
    }
}
