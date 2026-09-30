#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/http/http_client.hpp>
#include "../test_main.cpp"

#include <array>
#include <atomic>
#include <cerrno>
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
    typed_empty, length_only, type_only, payload, bodyless, legacy_empty, value_empty,
    raw_empty
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

struct hop_diagnostics {
    std::chrono::steady_clock::time_point start;
    int64_t accepted_us = 0;
    int64_t last_read_us = 0;
    int fd = -1;
    std::string endpoints;
    std::string wire;
    std::string phase;
    elio::io::io_result last_read{};
    elio::io::io_result last_write{};
};

std::string socket_endpoints(int fd) {
    sockaddr_storage local{}, peer{};
    socklen_t local_size = sizeof(local), peer_size = sizeof(peer);
    const auto local_result = ::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_size);
    const auto peer_result = ::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_size);
    return (local_result == 0 ? elio::net::socket_address(local).to_string() : "local_error") +
        " -> " + (peer_result == 0 ? elio::net::socket_address(peer).to_string() : "peer_error");
}

struct empty_fixture_connection final : std::exception {};

task<request> receive_request(elio::net::tcp_stream& stream, elio::coro::cancel_token token,
                              hop_diagnostics& diagnostics, bool headers_only = false) {
    elio::http::request_parser parser;
    std::array<char, 1024> buffer{};
    size_t received_bytes = 0;
    while (received_bytes < 32768) {
        const auto received = co_await stream.read(buffer.data(), buffer.size(), token);
        diagnostics.last_read = received;
        diagnostics.last_read_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - diagnostics.start).count();
        if (received.result == 0 && received_bytes == 0) throw empty_fixture_connection{};
        if (received.result <= 0) throw std::runtime_error("fixture request ended before completion");
        diagnostics.wire.append(buffer.data(), static_cast<size_t>(received.result));
        received_bytes += static_cast<size_t>(received.result);
        const auto [state, consumed] = parser.parse(
            std::string_view(buffer.data(), static_cast<size_t>(received.result)));
        (void)consumed;
        // This peer deliberately redirects malformed/body-incomplete first-hop
        // framing after its headers. The second hop always uses strict parsing.
        if (headers_only && (state == elio::http::parse_result::error ||
                             parser.declared_content_length().has_value())) {
            co_return request::from_parser(parser);
        }
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
                                  representation shape, std::string_view raw_length = {},
                                  bool inject_empty_peer = false) {
    CAPTURE(code, static_cast<int>(original_method), static_cast<int>(shape), raw_length);
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
        shape == representation::payload || shape == representation::raw_empty) {
        original.set_content_type(elio::http::mime::application_json);
    }
    if (shape == representation::raw_empty) original.set_header("Content-Length", raw_length);
    if (shape != representation::payload) original.set_expect_continue();
    elio::http::client_config config;
    config.read_timeout = elio::test::scaled_sec(2);
    elio::http::client client(config);
    elio::coro::cancel_source cleanup;
    const auto fixture_start = std::chrono::steady_clock::now();
    std::array<request, 2> requests;
    std::array<hop_diagnostics, 2> diagnostics;
    for (auto& hop : diagnostics) hop.start = fixture_start;
    std::exception_ptr server_failure;
    std::exception_ptr client_failure;
    std::exception_ptr probe_failure;
    std::atomic<bool> server_done{false};
    std::atomic<bool> client_done{false};
    std::atomic<bool> probe_done{!inject_empty_peer};
    std::atomic<unsigned> empty_peers{0};
    int status_code = 0;
    int client_error = 0;
    size_t accepted = 0;
    size_t processed = 0;
    elio::runtime::scheduler scheduler(2);
    scheduler.start();
    scheduler.go([&]() -> task<void> {
        try {
            for (size_t index = 0; index < requests.size();) {
                diagnostics[index].phase = "accept";
                auto stream = co_await listener->accept(cleanup.get_token());
                if (!stream) throw std::runtime_error("fixture accept cancelled");
                diagnostics[index].fd = stream->fd();
                diagnostics[index].accepted_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - fixture_start).count();
                diagnostics[index].endpoints = socket_endpoints(stream->fd());
                ++accepted;
                diagnostics[index].phase = "read";
                try {
                    requests[index] = co_await receive_request(*stream, cleanup.get_token(),
                        diagnostics[index], index == 0 && shape == representation::raw_empty);
                } catch (const empty_fixture_connection&) {
                    // An unrelated empty preconnection is not the next HTTP
                    // hop. Do not hide read errors or EOF after request bytes.
                    if (empty_peers.fetch_add(1, std::memory_order_acq_rel) >= 4) {
                        throw std::runtime_error("fixture empty-connection limit exceeded");
                    }
                    continue;
                }
                const auto response = index == 0
                    ? "HTTP/1.1 " + std::to_string(code) +
                      " Redirect\r\nLocation: /next\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                    : std::string("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                diagnostics[index].phase = "write";
                const auto sent = co_await stream->write_exactly(response, cleanup.get_token());
                diagnostics[index].last_write = sent;
                if (sent.result != static_cast<int32_t>(response.size())) {
                    throw std::runtime_error("fixture response write failed");
                }
                stream->shutdown_socket();
                diagnostics[index].phase = "complete";
                ++index;
                ++processed;
            }
        } catch (...) {
            server_failure = std::current_exception();
        }
        server_done.store(true, std::memory_order_release);
    });
    bool empty_peer_acknowledged = !inject_empty_peer;
    if (inject_empty_peer) {
        scheduler.go([&]() -> task<void> {
            try {
                auto probe = co_await elio::net::tcp_connect(listener->local_address(), cleanup.get_token());
                if (!probe) throw std::runtime_error("fixture preconnection failed");
                probe->shutdown_socket();
            } catch (...) { probe_failure = std::current_exception(); }
            probe_done.store(true, std::memory_order_release);
        });
        empty_peer_acknowledged = wait_for([&] {
            return probe_done.load(std::memory_order_acquire) &&
                   empty_peers.load(std::memory_order_acquire) > 0;
        });
    }
    scheduler.go([&]() -> task<void> {
        try {
            std::optional<elio::http::response> response;
            if (shape == representation::value_empty) {
                auto result = co_await client.request_result(original_method, endpoint, {},
                    elio::http::mime::application_json, cleanup.get_token());
                if (auto* value = std::get_if<elio::http::response>(&result)) response = std::move(*value);
                else client_error = std::get<elio::http::client_error>(result).code.value();
            } else if (shape == representation::legacy_empty) {
                response = co_await client.post(endpoint, {}, cleanup.get_token(),
                    elio::http::mime::application_json);
                if (!response) client_error = errno;
            } else {
                response = co_await client.send(original, *target, cleanup.get_token());
                if (!response) client_error = errno;
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
               server_done.load(std::memory_order_acquire) &&
               probe_done.load(std::memory_order_acquire);
    });
    cleanup.cancel();
    const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    INFO("client status=" << status_code << " error=" << client_error << " accepts=" << accepted
         << " empty peers=" << empty_peers.load());
    const auto hops = [&] {
        std::string detail;
        for (size_t index = 0; index < diagnostics.size(); ++index) {
            detail += "hop=" + std::to_string(index) + " phase=" + diagnostics[index].phase +
                " fd=" + std::to_string(diagnostics[index].fd) +
                " accepted_us=" + std::to_string(diagnostics[index].accepted_us) +
                " last_read_us=" + std::to_string(diagnostics[index].last_read_us) +
                " endpoints=" + diagnostics[index].endpoints +
                " read=" + std::to_string(diagnostics[index].last_read.result) +
                " write=" + std::to_string(diagnostics[index].last_write.result) +
                " wire=" + diagnostics[index].wire + "\n";
        }
        return detail;
    }();
    INFO(hops);
    REQUIRE(completed);
    if (server_failure) {
        try { std::rethrow_exception(server_failure); }
        catch (const std::exception& ex) { INFO(ex.what()); REQUIRE_FALSE(server_failure); }
    }
    if (client_failure) {
        try { std::rethrow_exception(client_failure); }
        catch (const std::exception& ex) { INFO(ex.what()); REQUIRE_FALSE(client_failure); }
    }
    REQUIRE_FALSE(server_failure);
    REQUIRE_FALSE(client_failure);
    REQUIRE_FALSE(probe_failure);
    REQUIRE(empty_peer_acknowledged);
    REQUIRE(processed == 2);
    REQUIRE(accepted == processed + empty_peers.load());
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

TEST_CASE("HTTP empty redirects preserve only a valid declared zero length",
          "[http][client][empty_redirect][raw_length]") {
    const int code = GENERATE(307, 308);
    const auto length = GENERATE(std::string("0"), std::string("000"),
        std::string("\t0\t"), std::string("4"), std::string("invalid"),
        std::string("+0"), std::string("0junk"), std::string("184467440737095516160"));
    CAPTURE(code, length);
    const auto requests = run_redirect(code, method::PUT, representation::raw_empty, length);
    REQUIRE(requests[0].get_method() == method::PUT);
    REQUIRE(requests[0].get_headers().contains("Content-Length"));
    REQUIRE(requests[0].body().empty());
    const bool valid_zero = requests[0].get_headers().content_length() == size_t{0};
    REQUIRE(requests[1].get_method() == method::PUT);
    REQUIRE(requests[1].get_headers().contains("Content-Length") == valid_zero);
    if (valid_zero) REQUIRE(requests[1].get_headers().content_length() == size_t{0});
    REQUIRE(requests[1].content_type() == elio::http::mime::application_json);
    REQUIRE(requests[1].body().empty());
    REQUIRE_FALSE(requests[1].get_headers().contains("Expect"));
}

TEST_CASE("HTTP redirect fixture tolerates a bounded unrelated empty preconnection",
          "[http][client][empty_redirect][fixture][regression]") {
    const auto requests = run_redirect(307, method::PUT, representation::typed_empty, {}, true);
    REQUIRE(requests[0].path() == "/start");
    REQUIRE(requests[1].path() == "/next");
    require_representation(requests[0], representation::typed_empty);
    require_representation(requests[1], representation::typed_empty);
}
