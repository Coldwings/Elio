#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/http/sse_writer.hpp>
#include <elio/http/http_response_sender.hpp>
#include <elio/http/http_parser.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace elio::http;

static_assert(!std::is_constructible_v<sse::event_writer, body_writer&>);

namespace {
struct sse_test_transport {
    std::string wire;
    size_t calls = 0;
    bool fail_body = false;
    std::function<void(const iovec*, size_t)> inspect;

    elio::coro::task<elio::io::io_result> writev(
        iovec* buffers, size_t count, elio::coro::cancel_token token) {
        ++calls;
        if (token.is_cancelled()) co_return elio::io::io_result{-ECANCELED, 0};
        if (fail_body && wire.find("\r\n\r\n") != std::string::npos) {
            co_return elio::io::io_result{-EPIPE, 0};
        }
        if (inspect) inspect(buffers, count);
        size_t written = 0;
        for (size_t i = 0; i < count && written < 7; ++i) {
            const auto amount = std::min(buffers[i].iov_len, 7 - written);
            if (amount) wire.append(static_cast<const char*>(buffers[i].iov_base), amount);
            written += amount;
        }
        co_return elio::io::io_result{static_cast<int32_t>(written), 0};
    }
};

template<typename T>
T complete_sse_task(elio::coro::task<T> operation) {
    auto handle = elio::coro::detail::task_access::handle(operation);
    handle.resume();
    REQUIRE(handle.done());
    return operation.await_resume();
}

std::string decoded_sse_body(const std::string& wire) {
    response_parser parser;
    REQUIRE(parser.parse(wire).first == parse_result::complete);
    REQUIRE(parser.bytes_remaining() == 0);
    return std::string(parser.body());
}
}

TEST_CASE("SSE writer borrows fields and normalizes multiline payload",
          "[http][sse][sse_writer]") {
    std::string id = "42";
    std::string type = "update";
    std::string data = "alpha\r\nbeta\rgamma\n";
    bool id_borrowed = false;
    bool type_borrowed = false;
    bool data_borrowed = false;
    sse_test_transport transport;
    transport.inspect = [&](const iovec* parts, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            id_borrowed |= parts[i].iov_base == id.data();
            type_borrowed |= parts[i].iov_base == type.data();
            data_borrowed |= parts[i].iov_base == data.data();
        }
    };
    reply selected = sse::make_streaming_response(
        [&](sse::event_writer& out, elio::coro::cancel_token token) -> elio::coro::task<send_result> {
            co_return co_await out.send_event({id, type, data, 123}, token);
        });
    auto sent = complete_sse_task(send_response(transport, selected, method::GET, "HTTP/1.1", true));
    REQUIRE(sent.success());
    REQUIRE(sent.reusable);
    REQUIRE(id_borrowed);
    REQUIRE(type_borrowed);
    REQUIRE(data_borrowed);
    REQUIRE(decoded_sse_body(transport.wire) ==
            "id: 42\nevent: update\nretry: 123\ndata: alpha\ndata: beta\ndata: gamma\ndata: \n\n");
    REQUIRE(transport.wire.find("Transfer-Encoding: chunked\r\n") != std::string::npos);
    REQUIRE(transport.wire.find("Content-Type: text/event-stream\r\n") != std::string::npos);
    REQUIRE(transport.wire.find("Cache-Control: no-cache\r\n") != std::string::npos);
    REQUIRE(transport.wire.find("Access-Control-Allow-Origin") == std::string::npos);
}

TEST_CASE("SSE writer emits empty fields comments and bounded multiline batches",
          "[http][sse][sse_writer]") {
    std::string data;
    std::string expected = "data: \n\n: first\n: second\n: \n\n";
    for (int i = 0; i < 100; ++i) {
        data += "x\n";
        expected += "data: x\n";
    }
    expected += "data: \n\n";
    sse_test_transport transport;
    transport.inspect = [](const iovec*, size_t count) {
        // Framing may add chunk header and CRLF descriptors to the fixed batch.
        REQUIRE(count <= 26);
    };
    reply selected = sse::make_streaming_response(
        [&](sse::event_writer& out, elio::coro::cancel_token token) -> elio::coro::task<send_result> {
            auto result = co_await out.send_data({}, token);
            if (!result.success()) co_return result;
            result = co_await out.send_comment("first\r\nsecond\n", token);
            if (!result.success()) co_return result;
            co_return co_await out.send_data(data, token);
        });
    REQUIRE(complete_sse_task(send_response(transport, selected, method::GET, "HTTP/1.1", true)).success());
    REQUIRE(decoded_sse_body(transport.wire) == expected);
}

TEST_CASE("SSE writer invalid fields fail before event output and stay terminal",
          "[http][sse][sse_writer]") {
    const auto bad = GENERATE(std::string("bad\rvalue"), std::string("bad\nvalue"), std::string("bad\0value", 9));
    const bool invalid_id = GENERATE(false, true);
    sse_test_transport transport;
    send_result first;
    send_result second;
    reply selected = sse::make_streaming_response(
        [&](sse::event_writer& out, elio::coro::cancel_token token) -> elio::coro::task<send_result> {
            first = co_await out.send_event(
                {invalid_id ? std::string_view(bad) : std::string_view{},
                 invalid_id ? std::string_view{} : std::string_view(bad), "must not send"}, token);
            second = co_await out.send_comment("also must not send", token);
            co_return send_result{}; // Ignoring the error must not complete the response.
        });
    const auto sent = complete_sse_task(send_response(transport, selected, method::GET, "HTTP/1.1", true));
    REQUIRE(first.error == send_errc::invalid_response);
    REQUIRE(first.transport_error == EINVAL);
    REQUIRE(second.error == first.error);
    REQUIRE(sent.result.error == first.error);
    REQUIRE_FALSE(sent.reusable);
    const auto end = transport.wire.find("\r\n\r\n");
    REQUIRE(end != std::string::npos);
    REQUIRE(transport.wire.size() == end + 4);
}

TEST_CASE("SSE writer propagates transport failure without later output",
          "[http][sse][sse_writer]") {
    sse_test_transport transport;
    transport.fail_body = true;
    size_t calls_after_failure = 0;
    send_result repeated;
    reply selected = sse::make_streaming_response(
        [&](sse::event_writer& out, elio::coro::cancel_token token) -> elio::coro::task<send_result> {
            const auto failed = co_await out.send_data("first", token);
            calls_after_failure = transport.calls;
            repeated = co_await out.send_data("second", token);
            co_return failed;
        });
    const auto sent = complete_sse_task(send_response(transport, selected, method::GET, "HTTP/1.1", true));
    REQUIRE(sent.result.error == send_errc::transport_error);
    REQUIRE(sent.result.transport_error == EPIPE);
    REQUIRE(repeated.error == sent.result.error);
    REQUIRE(transport.calls == calls_after_failure);
    REQUIRE_FALSE(sent.reusable);
}

TEST_CASE("SSE factory owns move-only producer and does not invoke it for HEAD",
          "[http][sse][sse_writer]") {
    const auto verb = GENERATE(method::GET, method::HEAD);
    auto owned = std::make_unique<std::string>("owned");
    int calls = 0;
    reply selected = sse::make_streaming_response(
        [payload = std::move(owned), &calls](sse::event_writer& out,
            elio::coro::cancel_token token) -> elio::coro::task<send_result> {
            ++calls;
            co_return co_await out.send_data(*payload, token);
        });
    REQUIRE_FALSE(owned);
    sse_test_transport transport;
    REQUIRE(complete_sse_task(send_response(transport, selected, verb, "HTTP/1.1", true)).success());
    REQUIRE(calls == (verb == method::HEAD ? 0 : 1));
    if (verb == method::GET) {
        REQUIRE(decoded_sse_body(transport.wire) == "data: owned\n\n");
        const auto previous = transport.wire;
        const auto second = complete_sse_task(send_response(transport, selected, verb, "HTTP/1.1", true));
        REQUIRE(second.result.error == send_errc::invalid_state);
        REQUIRE(transport.wire == previous);
    }
}
