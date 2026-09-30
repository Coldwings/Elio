#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/http/http_client.hpp>
#include "../test_main.cpp"

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>

namespace {
using elio::coro::task;
using elio::http::method;
using elio::http::request;

enum class representation {
    typed_empty, length_only, type_only, payload, bodyless, legacy_empty, value_empty
};

template<typename Predicate>
bool wait_for(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + elio::test::scaled_ms(5000);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

task<request> receive_request(elio::net::tcp_stream& stream, elio::coro::cancel_token token) {
    elio::http::request_parser parser;
    std::array<char, 1024> buffer{};
    size_t received_bytes = 0;
    while (received_bytes < 32768) {
        const auto received = co_await stream.read(buffer.data(), buffer.size(), token);
        if (received.result <= 0) throw std::runtime_error("fixture request ended before completion");
        received_bytes += static_cast<size_t>(received.result);
        const auto [state, consumed] = parser.parse(
            std::string_view(buffer.data(), static_cast<size_t>(received.result)));
        (void)consumed;
        if (state == elio::http::parse_result::error) {
            throw std::runtime_error("invalid fixture request");
        }
        if (state == elio::http::parse_result::complete) {
            co_return request::from_parser(parser);
        }
    }
    throw std::runtime_error("fixture request limit exceeded");
}

std::array<request, 2> run_redirect(int code, method original_method,
                                  representation shape) {
    auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    const auto endpoint = "http://127.0.0.1:" +
        std::to_string(listener->local_address().port()) + "/start";
    const auto target = elio::http::url::parse(endpoint);
    REQUIRE(target);
    request original(original_method, "/start");
    if (shape == representation::typed_empty || shape == representation::length_only) {
        original.set_body(std::string_view{});
    } else if (shape == representation::payload) {
        original.set_body(std::string_view("data"));
    }
    if (shape == representation::typed_empty || shape == representation::type_only ||
        shape == representation::payload) {
        original.set_content_type(elio::http::mime::application_json);
    }
    if (shape != representation::payload) original.set_expect_continue();
    elio::http::client_config config;
    config.read_timeout = elio::test::scaled_sec(2);
    elio::http::client client(config);
    elio::coro::cancel_source cleanup;
    std::array<request, 2> requests;
    std::exception_ptr server_failure;
    std::exception_ptr client_failure;
    std::atomic<bool> server_done{false};
    std::atomic<bool> client_done{false};
    int status_code = 0;
    size_t accepted = 0;
    elio::runtime::scheduler scheduler(2);
    scheduler.start();
    scheduler.go([&]() -> task<void> {
        try {
            for (size_t index = 0; index < requests.size(); ++index) {
                auto stream = co_await listener->accept(cleanup.get_token());
                if (!stream) throw std::runtime_error("fixture accept cancelled");
                ++accepted;
                requests[index] = co_await receive_request(*stream, cleanup.get_token());
                const auto response = index == 0
                    ? "HTTP/1.1 " + std::to_string(code) +
                      " Redirect\r\nLocation: /next\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                    : std::string("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                const auto sent = co_await stream->write_exactly(response, cleanup.get_token());
                if (!sent.success()) throw std::runtime_error("fixture response write failed");
                stream->shutdown_socket();
            }
        } catch (...) {
            server_failure = std::current_exception();
        }
        server_done.store(true, std::memory_order_release);
    });
    scheduler.go([&]() -> task<void> {
        try {
            std::optional<elio::http::response> response;
            if (shape == representation::value_empty) {
                auto result = co_await client.request_result(original_method, endpoint, {},
                    elio::http::mime::application_json, cleanup.get_token());
                if (auto* value = std::get_if<elio::http::response>(&result)) response = std::move(*value);
            } else if (shape == representation::legacy_empty) {
                response = co_await client.post(endpoint, {}, cleanup.get_token(),
                    elio::http::mime::application_json);
            } else {
                response = co_await client.send(original, *target, cleanup.get_token());
            }
            if (response) status_code = response->status_code();
        } catch (...) {
            client_failure = std::current_exception();
        }
        client_done.store(true, std::memory_order_release);
    });
    // Redirects can admit another watchdog; keep admission open until the
    // entire request has returned, not merely until the fixture sent a reply.
    const bool completed = wait_for([&] {
        return client_done.load(std::memory_order_acquire) &&
               server_done.load(std::memory_order_acquire);
    });
    cleanup.cancel();
    const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(completed);
    REQUIRE_FALSE(server_failure);
    REQUIRE_FALSE(client_failure);
    REQUIRE(accepted == 2);
    REQUIRE(status_code == 200);
    return requests;
}

void require_representation(const request& message, representation shape) {
    const bool typed = shape == representation::typed_empty ||
                       shape == representation::type_only || shape == representation::payload;
    const bool sized = shape == representation::typed_empty ||
                       shape == representation::length_only || shape == representation::payload;
    REQUIRE(message.get_headers().contains("Content-Type") == typed);
    if (typed) REQUIRE(message.content_type() == elio::http::mime::application_json);
    REQUIRE(message.get_headers().contains("Content-Length") == sized);
    if (sized) {
        REQUIRE(message.get_headers().content_length() ==
                (shape == representation::payload ? size_t{4} : size_t{0}));
    }
    REQUIRE(message.body() == (shape == representation::payload ? "data" : ""));
    REQUIRE_FALSE(message.get_headers().contains("Expect"));
}
} // namespace

TEST_CASE("HTTP method-preserving redirects retain explicit representation metadata",
          "[http][client][empty_redirect]") {
    const int code = GENERATE(301, 302, 307, 308);
    const auto shape = GENERATE(representation::typed_empty, representation::length_only,
                               representation::type_only, representation::payload,
                               representation::bodyless);
    const auto requests = run_redirect(code, method::PUT, shape);
    REQUIRE(requests[0].get_method() == method::PUT);
    REQUIRE(requests[1].get_method() == method::PUT);
    REQUIRE(requests[0].path() == "/start");
    REQUIRE(requests[1].path() == "/next");
    require_representation(requests[0], shape);
    require_representation(requests[1], shape);
}

TEST_CASE("HTTP POST redirects preserve or drop empty representations according to method policy",
          "[http][client][empty_redirect]") {
    const int code = GENERATE(301, 302, 303, 307, 308);
    const auto shape = GENERATE(representation::typed_empty, representation::bodyless,
                               representation::legacy_empty, representation::value_empty);
    const auto requests = run_redirect(code, method::POST, shape);
    const auto original_shape = shape == representation::legacy_empty ? representation::bodyless :
        shape == representation::value_empty ? representation::typed_empty : shape;
    const bool preserved = code == 307 || code == 308;
    REQUIRE(requests[0].get_method() == method::POST);
    REQUIRE(requests[1].get_method() == (preserved ? method::POST : method::GET));
    require_representation(requests[0], original_shape);
    require_representation(requests[1], preserved ? original_shape : representation::bodyless);
}

TEST_CASE("HTTP 303 discards representations while retaining HEAD method semantics",
          "[http][client][empty_redirect]") {
    const auto original_method = GENERATE(method::PUT, method::HEAD);
    const auto requests = run_redirect(303, original_method, representation::typed_empty);
    REQUIRE(requests[0].get_method() == original_method);
    REQUIRE(requests[1].get_method() == (original_method == method::HEAD ? method::HEAD : method::GET));
    require_representation(requests[0], representation::typed_empty);
    require_representation(requests[1], representation::bodyless);
}
