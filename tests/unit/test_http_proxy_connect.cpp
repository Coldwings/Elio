#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/http/detail/proxy_connect.hpp>
#include <elio/http/detail/owned_prefix_stream.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/sync/event.hpp>
#include <elio/time/timer.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

using namespace elio::http;
using elio::coro::task;

namespace {

template<typename T>
T immediate(task<T> operation) {
    auto handle = elio::coro::detail::task_access::handle(operation);
    handle.resume();
    REQUIRE(handle.done());
    return operation.await_resume();
}

struct scripted_proxy {
    std::string input;
    std::string output;
    size_t offset = 0;
    size_t read_step = std::numeric_limits<size_t>::max();
    size_t write_step = std::numeric_limits<size_t>::max();
    bool pause_read = false;
    bool pause_write = false;
    elio::sync::event entered;
    elio::sync::event release;

    task<elio::io::io_result> read(void* data, size_t size, elio::coro::cancel_token token) {
        if (pause_read) {
            entered.set();
            if (co_await release.wait(token) == elio::coro::cancel_result::cancelled)
                co_return elio::io::io_result{-ECANCELED, 0};
        }
        const auto count = std::min({size, read_step, input.size() - offset});
        std::memcpy(data, input.data() + offset, count);
        offset += count;
        co_return elio::io::io_result{static_cast<int>(count), 0};
    }
    task<elio::io::io_result> write(const void* data, size_t size, elio::coro::cancel_token token) {
        if (pause_write) {
            entered.set();
            if (co_await release.wait(token) == elio::coro::cancel_result::cancelled)
                co_return elio::io::io_result{-ECANCELED, 0};
        }
        const auto count = std::min(size, write_step);
        output.append(static_cast<const char*>(data), count);
        co_return elio::io::io_result{static_cast<int>(count), 0};
    }
};

auto profile() {
    http_proxy_config config;
    config.endpoint = "http://proxy.example:8080";
    config.basic_auth = proxy_basic_credentials{"hop", "secret"};
    return detail::freeze_proxy_profile(config);
}

void check_error(const client_result<std::vector<char>>& result, int expected) {
    REQUIRE(std::holds_alternative<client_error>(result));
    CHECK(std::get<client_error>(result).code.value() == expected);
    CHECK(std::get<client_error>(result).stage == client_stage::proxy_connect);
}

} // namespace

TEST_CASE("CONNECT negotiation uses authority form and transfers owned read-ahead exactly once",
          "[http][proxy][connect][issue-1249]") {
    const auto read_step = GENERATE(size_t{1}, size_t{3}, size_t{8192});
    scripted_proxy stream;
    stream.read_step = read_step;
    stream.write_step = 3;
    stream.input = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 Connected\r\n"
                   "Content-Length: invalid-but-ignored\r\nTransfer-Encoding: unknown\r\n\r\nTLS";
    auto result = immediate(detail::negotiate_connect(stream,
        detail::route_endpoint::from("2001:0db8::1", 443), *profile()));
    REQUIRE(std::holds_alternative<std::vector<char>>(result));
    const auto prefix = std::get<std::vector<char>>(result);
    REQUIRE(stream.output.starts_with("CONNECT [2001:db8::1]:443 HTTP/1.1\r\n"));
    REQUIRE(stream.output.find("Host: [2001:db8::1]:443\r\n") != std::string::npos);
    REQUIRE(stream.output.find("Proxy-Authorization: Basic aG9wOnNlY3JldA==\r\n") != std::string::npos);
    REQUIRE(stream.output.ends_with("\r\n\r\n"));
    std::string owned(prefix.begin(), prefix.end());
    std::array<char, 3> following{};
    auto rest = immediate(stream.read(following.data(), following.size(), {}));
    owned.append(following.data(), static_cast<size_t>(rest.result));
    while (owned.size() < 3) {
        rest = immediate(stream.read(following.data(), following.size(), {}));
        REQUIRE(rest.result > 0);
        owned.append(following.data(), static_cast<size_t>(rest.result));
    }
    REQUIRE(owned == "TLS");
    REQUIRE(immediate(stream.read(following.data(), following.size(), {})).result == 0);
}

TEST_CASE("CONNECT negotiation rejects terminal proxy replies without replaying credentials",
          "[http][proxy][connect][issue-1249]") {
    const auto status = GENERATE(407, 403, 502, 101);
    scripted_proxy stream;
    stream.input = "HTTP/1.1 " + std::to_string(status) + " Rejected\r\n"
                   "Content-Length: 999999\r\n\r\n";
    auto result = immediate(detail::negotiate_connect(stream,
        detail::route_endpoint::from("origin.example", 443), *profile()));
    check_error(result, status == 407 ? EACCES : status == 101 ? ENOTSUP : ECONNREFUSED);
    REQUIRE(stream.output.find("CONNECT", 1) == std::string::npos);
}

TEST_CASE("Successful CONNECT ignores all repeated framing fields before handing off",
          "[http][proxy][connect][review-1249][issue-1249]") {
    const auto status = GENERATE(200, 204, 299);
    scripted_proxy stream;
    stream.input = "HTTP/1.1 " + std::to_string(status) + " Tunnel\r\n"
                   "Content-Length: 0\r\nContent-Length: 1\r\n"
                   "Transfer-Encoding: unknown\r\nTransfer-Encoding: chunked\r\n\r\nTLS";
    auto result = immediate(detail::negotiate_connect(stream,
        detail::route_endpoint::from("origin.example", 443), *profile()));
    REQUIRE(std::holds_alternative<std::vector<char>>(result));
    const auto& prefix = std::get<std::vector<char>>(result);
    REQUIRE(std::string(prefix.begin(), prefix.end()) == "TLS");
}

TEST_CASE("CONNECT rejects out-of-range status codes rather than treating them as interim",
          "[http][proxy][connect][review-1249][issue-1249]") {
    const auto status = GENERATE("000", "099", "600", "999");
    scripted_proxy stream;
    stream.input = "HTTP/1.1 " + std::string(status) + " Invalid\r\nContent-Length: 0\r\n\r\n"
                   "HTTP/1.1 200 Tunnel\r\n\r\nTLS";
    auto result = immediate(detail::negotiate_connect(stream,
        detail::route_endpoint::from("origin.example", 443), *profile()));
    check_error(result, EBADMSG);
}

TEST_CASE("CONNECT zero read-ahead leaves subsequent tunnel bytes on the stream",
          "[http][proxy][connect][issue-1249]") {
    scripted_proxy stream;
    stream.input = "HTTP/1.1 200 Tunnel\r\n\r\nTLS";
    auto proxy = *profile();
    proxy.limits.max_read_ahead = 0;
    auto result = immediate(detail::negotiate_connect(stream,
        detail::route_endpoint::from("origin.example", 443), proxy));
    REQUIRE(std::holds_alternative<std::vector<char>>(result));
    REQUIRE(std::get<std::vector<char>>(result).empty());
    std::array<char, 3> following{};
    REQUIRE(immediate(stream.read(following.data(), following.size(), {})).result == 3);
    REQUIRE(std::string(following.data(), following.size()) == "TLS");
}

TEST_CASE("Successful CONNECT framing exceptions preserve metadata limits",
          "[http][proxy][connect][issue-1249]") {
    scripted_proxy stream;
    stream.input = "HTTP/1.1 200 Tunnel\r\nContent-Length: 0\r\nContent-Length: 1\r\n\r\nTLS";
    auto proxy = *profile();
    proxy.limits.max_headers = 1;
    auto result = immediate(detail::negotiate_connect(stream,
        detail::route_endpoint::from("origin.example", 443), proxy));
    check_error(result, EMSGSIZE);
}

TEST_CASE("CONNECT negotiation bounds response bytes metadata informational replies and zero writes",
          "[http][proxy][connect][issue-1249]") {
    const auto limit = GENERATE(0, 1, 2, 3, 4);
    scripted_proxy stream;
    detail::proxy_profile proxy = *profile();
    stream.input = "HTTP/1.1 200 Connected\r\nX-Header: value\r\n\r\n";
    switch (limit) {
    case 0: proxy.limits.max_response_bytes = 8; break;
    case 1: proxy.limits.max_headers = 0; break;
    case 2: proxy.limits.max_header_size = 8; break;
    case 3:
        proxy.limits.max_informational_responses = 0;
        stream.input = "HTTP/1.1 100 Continue\r\n\r\n" + stream.input;
        break;
    case 4: stream.write_step = 0; break;
    }
    auto result = immediate(detail::negotiate_connect(stream,
        detail::route_endpoint::from("origin.example", 443), proxy));
    check_error(result, limit == 4 ? EIO : EMSGSIZE);
}

TEST_CASE("CONNECT negotiation reports malformed response and already expired absolute setup budget",
          "[http][proxy][connect][issue-1249]") {
    scripted_proxy stream;
    stream.input = "HTTP/1.1 bad status\r\n\r\n";
    const auto target = detail::route_endpoint::from("origin.example", 443);
    auto proxy = profile();
    check_error(immediate(detail::negotiate_connect(stream, target, *proxy)), EBADMSG);
    stream.output.clear();
    check_error(immediate(detail::negotiate_connect(stream, target, *proxy, {},
        std::chrono::steady_clock::now())), ETIMEDOUT);
    REQUIRE(stream.output.empty());
}

TEST_CASE("CONNECT negotiation cancellation settles pending request writes and response reads",
          "[http][proxy][connect][issue-1249]") {
    const bool write = GENERATE(false, true);
    scripted_proxy stream;
    stream.pause_read = !write;
    stream.pause_write = write;
    const auto target = detail::route_endpoint::from("origin.example", 443);
    auto proxy = profile();
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto operation = scheduler.go_joinable([&]() -> task<void> {
        elio::coro::cancel_source stop;
        std::optional<elio::coro::join_handle<client_result<std::vector<char>>>> negotiating;
        std::exception_ptr failure;
        bool entered = false;
        try {
            negotiating.emplace(scheduler.go_joinable(
                detail::negotiate_connect(stream, target, *proxy, stop.get_token())));
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(5);
            while (!stream.entered.is_set() && !negotiating->is_ready() &&
                   std::chrono::steady_clock::now() < deadline) {
                co_await elio::time::yield();
            }
            entered = stream.entered.is_set();
            if (!entered && !negotiating->is_ready())
                throw std::runtime_error("CONNECT negotiation did not reach its pause marker");
        } catch (...) {
            failure = std::current_exception();
        }
        stop.cancel();
        std::optional<client_result<std::vector<char>>> result;
        if (negotiating) {
            auto& joined = *negotiating;
            try { result.emplace(co_await joined); }
            catch (...) { if (!failure) failure = std::current_exception(); }
            try { co_await negotiating->wait_destroyed_async(); }
            catch (...) { if (!failure) failure = std::current_exception(); }
        }
        if (failure) std::rethrow_exception(failure);
        REQUIRE(entered);
        REQUIRE(result);
        check_error(*result, ECANCELED);
    });
    operation.wait_destroyed();
    scheduler.shutdown();
    operation.await_resume();
}
