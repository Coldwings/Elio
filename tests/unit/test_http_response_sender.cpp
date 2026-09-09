#include <catch2/catch_test_macros.hpp>
#include <elio/http/http_response_sender.hpp>

#include <cstring>
#include <functional>
#include <stdexcept>

using namespace elio::http;

namespace {
struct observing_stream {
    std::string wire;
    size_t max_progress = 2;
    std::function<void(const iovec*, size_t)> inspect;

    elio::coro::task<elio::io::io_result> writev(
        iovec* parts, size_t count, elio::coro::cancel_token token) {
        if (token.is_cancelled()) co_return elio::io::io_result{-ECANCELED, 0};
        if (inspect) inspect(parts, count);
        size_t written = 0;
        for (size_t i = 0; i < count && written < max_progress; ++i) {
            const auto amount = std::min(parts[i].iov_len, max_progress - written);
            if (amount) wire.append(static_cast<const char*>(parts[i].iov_base), amount);
            written += amount;
        }
        co_return elio::io::io_result{static_cast<int32_t>(written), 0};
    }
};

template<typename T>
T run_inline(elio::coro::task<T> operation) {
    const auto handle = elio::coro::detail::task_access::handle(operation);
    handle.resume();
    REQUIRE(handle.done());
    return operation.await_resume();
}

std::string_view body_wire(const observing_stream& stream) {
    auto boundary = stream.wire.find("\r\n\r\n");
    REQUIRE(boundary != std::string::npos);
    return std::string_view(stream.wire).substr(boundary + 4);
}
}

TEST_CASE("response sender borrows complete body through partial writes",
          "[http][response_sender]") {
    reply selected = response::ok("abcdef");
    const auto original = std::get<response>(selected).body();
    observing_stream stream;
    bool borrowed = false;
    stream.inspect = [&](const iovec* parts, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            if (parts[i].iov_base == original.data()) borrowed = true;
        }
    };
    auto result = run_inline(send_response(stream, selected, method::GET, "HTTP/1.1", true));
    REQUIRE(result.success());
    REQUIRE(result.reusable);
    REQUIRE(result.result.confirmed_body_bytes == 6);
    REQUIRE(borrowed);
    REQUIRE(body_wire(stream) == original);
    REQUIRE(stream.wire.find("Content-Length: 6\r\n") != std::string::npos);
}

TEST_CASE("response sender owns exactly one producer and chunk finalization",
          "[http][response_sender]") {
    int calls = 0;
    auto producer = [owned = std::make_unique<std::string>("hello"), &calls](
        body_writer& writer, elio::coro::cancel_token token) -> elio::coro::task<send_result> {
        ++calls;
        co_return co_await writer.write(*owned, token);
    };
    reply selected = streaming_response(status::ok, std::move(producer));
    observing_stream stream;
    auto result = run_inline(send_response(stream, selected, method::GET, "HTTP/1.1", true));
    REQUIRE(result.success());
    REQUIRE(result.reusable);
    REQUIRE(calls == 1);
    REQUIRE(result.result.confirmed_body_bytes == 5);
    REQUIRE(body_wire(stream) == "5\r\nhello\r\n0\r\n\r\n");
    const auto wire = stream.wire;
    result = run_inline(send_response(stream, selected, method::GET, "HTTP/1.1", true));
    REQUIRE(result.result.error == send_errc::invalid_state);
    REQUIRE_FALSE(result.reusable);
    REQUIRE(stream.wire == wire);
    REQUIRE(calls == 1);
}

TEST_CASE("response sender suppresses HEAD production and validates preflight",
          "[http][response_sender]") {
    int calls = 0;
    auto producer = [&](body_writer&, elio::coro::cancel_token) -> elio::coro::task<send_result> {
        ++calls;
        co_return send_result{};
    };
    reply selected = streaming_response(status::ok, producer, 5);
    observing_stream stream;
    auto result = run_inline(send_response(stream, selected, method::HEAD, "HTTP/1.1", true));
    REQUIRE(result.success());
    REQUIRE(result.reusable);
    REQUIRE(calls == 0);
    REQUIRE(body_wire(stream).empty());
    REQUIRE(stream.wire.find("Content-Length: 5\r\n") != std::string::npos);

    selected = response::ok("hi");
    std::get<response>(selected).set_header("Content-Length", "3");
    stream.wire.clear();
    result = run_inline(send_response(stream, selected, method::GET, "HTTP/1.1", true));
    REQUIRE(result.result.error == send_errc::invalid_response);
    REQUIRE_FALSE(result.reusable);
    REQUIRE(stream.wire.empty());
}

TEST_CASE("response sender never finalizes failed production normally",
          "[http][response_sender]") {
    SECTION("producer exception") {
        auto producer = [](body_writer& writer, elio::coro::cancel_token) -> elio::coro::task<send_result> {
            auto sent = co_await writer.write("hi");
            if (!sent.success()) co_return sent;
            throw std::runtime_error("source failed");
        };
        reply selected = streaming_response(status::ok, producer);
        observing_stream stream;
        auto result = run_inline(send_response(stream, selected, method::GET, "HTTP/1.1", true));
        REQUIRE(result.result.error == send_errc::producer_error);
        REQUIRE(result.result.confirmed_body_bytes == 2);
        REQUIRE_FALSE(result.reusable);
        REQUIRE(body_wire(stream) == "2\r\nhi\r\n");
    }
    SECTION("known length underproduction") {
        auto producer = [](body_writer& writer, elio::coro::cancel_token) -> elio::coro::task<send_result> {
            co_return co_await writer.write("hi");
        };
        reply selected = streaming_response(status::ok, producer, 3);
        observing_stream stream;
        auto result = run_inline(send_response(stream, selected, method::GET, "HTTP/1.1", true));
        REQUIRE(result.result.error == send_errc::length_mismatch);
        REQUIRE_FALSE(result.reusable);
        REQUIRE(body_wire(stream) == "hi");
    }
    SECTION("ignored writer error remains terminal") {
        auto producer = [](body_writer& writer, elio::coro::cancel_token) -> elio::coro::task<send_result> {
            (void)co_await writer.write("too much");
            co_return send_result{};
        };
        reply selected = streaming_response(status::ok, producer, 1);
        observing_stream stream;
        auto result = run_inline(send_response(stream, selected, method::GET, "HTTP/1.1", true));
        REQUIRE(result.result.error == send_errc::length_mismatch);
        REQUIRE_FALSE(result.reusable);
        REQUIRE(body_wire(stream).empty());
    }
}
