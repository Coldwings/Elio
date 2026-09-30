#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/http/http_client.hpp>
#include <elio/io/file_helpers.hpp>
#include <elio/sync/event.hpp>
#include "../test_main.cpp"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <exception>
#include <thread>
#include <type_traits>

namespace {
using elio::coro::task;
using elio::http::client_error;
using elio::http::client_result;
using elio::http::client_stage;
using elio::http::response;

static_assert(std::is_trivially_copyable_v<client_error>);
static_assert(sizeof(client_error) <= 32);

bool wait_for(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + elio::test::scaled_ms(5000);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

template<typename T>
T run_immediate(task<T> operation) {
    auto handle = elio::coro::detail::task_access::handle(operation);
    handle.resume();
    REQUIRE(handle.done());
    return operation.await_resume();
}

client_error require_failure(const client_result<response>& result, int error, client_stage stage) {
    REQUIRE(std::holds_alternative<client_error>(result));
    const auto failure = std::get<client_error>(result);
    REQUIRE(failure.code == std::error_code(error, std::generic_category()));
    REQUIRE(failure.stage == stage);
    return failure;
}

task<std::string> read_headers(elio::net::tcp_stream& stream, elio::coro::cancel_token token) {
    std::string request;
    std::array<char, 1024> buffer{};
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
        const auto received = co_await stream.read(buffer.data(), buffer.size(), token);
        if (received.result <= 0) break;
        request.append(buffer.data(), static_cast<size_t>(received.result));
    }
    co_return request;
}

enum class request_api { get, method_general, custom };

std::atomic<elio::coro::cancel_source*> interim_cancel_source{nullptr};
void cancel_after_interim_headers(uint16_t code) {
    if (code == 100 || code == 103) {
        if (auto* source = interim_cancel_source.load(std::memory_order_acquire)) source->cancel();
    }
}
struct interim_cancel_guard {
    interim_cancel_guard(elio::coro::cancel_source& source, bool enabled) : enabled_(enabled) {
        if (enabled_) {
            interim_cancel_source.store(&source);
            elio::http::detail::response_headers_for_test.store(cancel_after_interim_headers);
        }
    }
    ~interim_cancel_guard() {
        if (enabled_) {
            elio::http::detail::response_headers_for_test.store(nullptr);
            interim_cancel_source.store(nullptr);
        }
    }
    bool enabled_;
};

client_result<response> run_wire_response(std::string_view wire,
        const std::function<void(elio::http::client_config&)>& configure = {},
        bool secure = false, std::exception_ptr* raised = nullptr,
        bool cancel_interim = false, request_api api = request_api::get,
        std::string* captured_request = nullptr) {
    auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    const auto target = std::string(secure ? "https://127.0.0.1:" : "http://127.0.0.1:") +
        std::to_string(listener->local_address().port()) + "/";
    elio::http::client_config config;
    config.read_timeout = std::chrono::seconds(2);
    if (configure) configure(config);
    elio::http::client client(config);
    elio::coro::cancel_source cleanup;
    interim_cancel_guard interim(cleanup, cancel_interim);
    client_result<response> result;
    bool accepted = false;
    std::atomic<bool> client_done{false};
    std::exception_ptr failure;
    elio::runtime::scheduler scheduler(2);
    scheduler.start();
    scheduler.go([&]() -> task<void> {
        auto stream = co_await listener->accept(cleanup.get_token());
        accepted = stream.has_value();
        if (!stream) co_return;
        if (!secure) {
            auto request = co_await read_headers(*stream, cleanup.get_token());
            if (captured_request) *captured_request = std::move(request);
        }
        (void)co_await stream->write_exactly(wire, cleanup.get_token());
        stream->shutdown_socket();
    });
    scheduler.go([&]() -> task<void> {
        try {
            if (api == request_api::method_general) {
                result = co_await client.request_result(elio::http::method::POST, target,
                    "data", elio::http::mime::text_plain, cleanup.get_token());
            } else if (api == request_api::custom) {
                elio::http::request request(elio::http::method::POST, "/");
                request.set_body(std::string_view("data"));
                request.set_content_type(elio::http::mime::text_plain);
                auto parsed = elio::http::url::parse(target);
                if (!parsed) throw std::logic_error("invalid fixture URL");
                result = co_await client.send_result(request, *parsed, cleanup.get_token());
            } else {
                result = co_await client.get_result(target, cleanup.get_token());
            }
        } catch (...) {
            failure = std::current_exception();
        }
        client_done.store(true, std::memory_order_release);
    });
    // Keep admission open until the request has finished creating watchdogs.
    const bool completed = wait_for([&] { return client_done.load(std::memory_order_acquire); });
    cleanup.cancel();
    const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(completed);
    if (raised) *raised = failure;
    else REQUIRE_FALSE(failure);
    REQUIRE(accepted);
    REQUIRE(client_done.load(std::memory_order_acquire));
    return result;
}

struct response_observer_guard {
    response_observer_guard() {
        elio::http::detail::client_response_read_staged_for_test.store(false);
        elio::http::detail::observe_client_response_read_entry_for_test.store(true);
    }
    ~response_observer_guard() {
        elio::http::detail::observe_client_response_read_entry_for_test.store(false);
    }
};

client_result<response> run_stalled_response(bool body, bool cancel) {
    auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    const auto target = "http://127.0.0.1:" + std::to_string(listener->local_address().port()) + "/";
    elio::http::client_config config;
    config.read_timeout = cancel ? std::chrono::seconds::zero() : std::chrono::seconds(1);
    elio::http::client client(config);
    elio::coro::cancel_source cleanup;
    elio::sync::event release_server;
    response_observer_guard observer;
    client_result<response> result;
    std::exception_ptr failure;
    std::atomic<bool> done{false};
    elio::runtime::scheduler scheduler(2);
    scheduler.start();
    scheduler.go([&]() -> task<void> {
        auto stream = co_await listener->accept(cleanup.get_token());
        if (!stream) co_return;
        co_await read_headers(*stream, cleanup.get_token());
        if (body) {
            (void)co_await stream->write_exactly(
                "HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nx", cleanup.get_token());
        }
        (void)co_await release_server.wait(cleanup.get_token());
    });
    scheduler.go([&]() -> task<void> {
        try { result = co_await client.get_result(target, cleanup.get_token()); }
        catch (...) { failure = std::current_exception(); }
        done.store(true, std::memory_order_release);
    });
    const auto expected_stage = body ? client_stage::body : client_stage::headers;
    const bool staged = wait_for([&] {
        return elio::http::detail::response_read_stage_for_test.load(std::memory_order_acquire) ==
            expected_stage && elio::http::detail::client_response_read_staged_for_test.load(
                std::memory_order_acquire);
    });
    if (cancel) cleanup.cancel();
    const bool completed = wait_for([&] { return done.load(std::memory_order_acquire); });
    cleanup.cancel();
    release_server.set();
    const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(staged);
    REQUIRE(completed);
    REQUIRE_FALSE(failure);
    return result;
}

elio::io::io_result fail_request_write(std::string_view) { return {-EPIPE, 0}; }
elio::io::io_result throw_request_write(std::string_view) { throw std::bad_alloc(); }
struct write_failure_guard {
    explicit write_failure_guard(elio::http::detail::request_write_hook hook = fail_request_write) {
        elio::http::detail::request_write_result_for_test.store(hook);
    }
    ~write_failure_guard() { elio::http::detail::request_write_result_for_test.store(nullptr); }
};
} // namespace

TEST_CASE("HTTP result errors own bounded target metadata", "[http][http_client_result]") {
    client_result<response> result;
    {
        elio::http::client client;
        std::string input = "unsupported://user:secret@host/path?token=private";
        result = run_immediate(client.get_result(input));
    }
    const auto saved = require_failure(result, EINVAL, client_stage::target);
    errno = ERANGE;
    REQUIRE(saved.code.value() == EINVAL);
    REQUIRE(saved.stage == client_stage::target);
    REQUIRE(sizeof(saved) <= 32);
}

TEST_CASE("HTTP optional adapters preserve errno failure mapping", "[http][http_client_result]") {
    elio::http::client client;
    errno = ERANGE;
    const auto result = run_immediate(client.get("unsupported://host/"));
    const int observed = errno;
    REQUIRE_FALSE(result);
    REQUIRE(observed == EINVAL);
}

TEST_CASE("HTTP result pre-cancellation is explicit", "[http][http_client_result][cancel]") {
    elio::coro::cancel_source source;
    source.cancel();
    elio::http::client client;
    const auto result = run_immediate(client.get_result("http://127.0.0.1/", source.get_token()));
    require_failure(result, ECANCELED, client_stage::target);
}

TEST_CASE("HTTP pool pre-cancellation has acquisition stage", "[http][http_client_result][cancel]") {
    elio::coro::cancel_source source;
    source.cancel();
    elio::http::connection_pool pool;
    const auto result = run_immediate(pool.acquire_result("127.0.0.1", 80, false,
        nullptr, {}, source.get_token()));
    REQUIRE(std::holds_alternative<client_error>(result));
    REQUIRE(std::get<client_error>(result).code.value() == ECANCELED);
    REQUIRE(std::get<client_error>(result).stage == client_stage::acquire);
}

TEST_CASE("HTTP cached acquisition preserves completion before pre-cancellation",
          "[http][http_client_result][cancel][pool]") {
    const bool legacy = GENERATE(false, true);
    std::array<int, 2> sockets{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets.data()) == 0);
    elio::net::tcp_stream cached(sockets[0]);
    elio::io::fd_guard peer(sockets[1]);
    elio::http::connection_pool pool;
    const int original = cached.fd();
    pool.release("cached.invalid", 80, false, elio::net::stream(std::move(cached)));
    elio::coro::cancel_source source;
    source.cancel();
    if (legacy) {
        const auto result = run_immediate(pool.acquire("cached.invalid", 80, false,
            nullptr, {}, source.get_token()));
        REQUIRE(result);
        REQUIRE(result->fd() == original);
    } else {
        const auto result = run_immediate(pool.acquire_result("cached.invalid", 80, false,
            nullptr, {}, source.get_token()));
        REQUIRE(std::holds_alternative<elio::net::stream>(result));
        REQUIRE(std::get<elio::net::stream>(result).fd() == original);
    }
}

TEST_CASE("HTTP request setup validation returns owned errors", "[http][http_client_result]") {
    elio::http::client_config config;
    config.user_agent = "agent\r\nInjected: value";
    elio::http::client client(config);
    const auto result = run_immediate(client.get_result("http://127.0.0.1/"));
    require_failure(result, EINVAL, client_stage::request);
}

TEST_CASE("HTTP result preserves a cached DNS error and its boundary", "[http][http_client_result][dns]") {
    elio::net::resolve_cache cache;
    cache.store({"dns-error.invalid", 80}, {}, std::chrono::seconds(30), EHOSTUNREACH);
    elio::http::client_config config;
    config.resolve_options.use_cache = true;
    config.resolve_options.cache = &cache;
    elio::http::client client(config);
    client_result<response> result;
    bool done = false;
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    scheduler.go([&]() -> task<void> {
        result = co_await client.get_result("http://dns-error.invalid/");
        done = true;
    });
    REQUIRE(scheduler.shutdown(elio::test::scaled_ms(5000)));
    REQUIRE(done);
    const auto failure = require_failure(result, EHOSTUNREACH, client_stage::resolve);
    errno = ENOSPC;
    REQUIRE(failure.code.value() == EHOSTUNREACH);
}

TEST_CASE("HTTP connection result preserves connect errors through cleanup", "[http][http_client_result][connect]") {
    client_result<elio::net::stream> result =
        elio::http::detail::make_client_error(EINPROGRESS, client_stage::acquire);
    int legacy_error = 0;
    bool legacy_failed = false;
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    scheduler.go([&]() -> task<void> {
        // TCP port zero cannot have a listening socket; no live DNS is used.
        result = co_await elio::http::client_connect_result("127.0.0.1", 0, false, nullptr);
        const auto legacy = co_await elio::http::client_connect("127.0.0.1", 0, false, nullptr);
        legacy_error = errno;
        legacy_failed = !legacy;
    });
    REQUIRE(scheduler.shutdown(elio::test::scaled_ms(5000)));
    REQUIRE(std::holds_alternative<client_error>(result));
    const auto failure = std::get<client_error>(result);
    REQUIRE(failure.stage == client_stage::connect);
    REQUIRE(failure.code == std::error_code(ECONNREFUSED, std::generic_category()));
    REQUIRE(legacy_failed);
    REQUIRE(legacy_error == failure.code.value());
}

TEST_CASE("HTTP non-success status remains a response value", "[http][http_client_result]") {
    const auto result = run_wire_response(
        "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 3\r\nConnection: close\r\n\r\nbus");
    REQUIRE(std::holds_alternative<response>(result));
    REQUIRE(std::get<response>(result).get_status() == elio::http::status::service_unavailable);
    REQUIRE(std::get<response>(result).body() == "bus");
}

TEST_CASE("HTTP custom and method-general value APIs send requests", "[http][http_client_result]") {
    const auto api = GENERATE(request_api::method_general, request_api::custom);
    std::string captured;
    const auto result = run_wire_response(
        "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        {}, false, nullptr, false, api, &captured);
    REQUIRE(std::holds_alternative<response>(result));
    REQUIRE(std::get<response>(result).status_code() == 401);
    REQUIRE(captured.starts_with("POST / HTTP/1.1\r\n"));
    REQUIRE(captured.find("Content-Type: text/plain\r\n") != std::string::npos);
    REQUIRE(captured.find("Content-Length: 4\r\n") != std::string::npos);
}

TEST_CASE("HTTP interim headers do not establish body cancellation stage",
          "[http][http_client_result][cancel][interim]") {
    const auto code = GENERATE(100, 103);
    const auto wire = "HTTP/1.1 " + std::to_string(code) + " Interim\r\n\r\n";
    const auto result = run_wire_response(wire, {}, false, nullptr, true);
    require_failure(result, ECANCELED, client_stage::headers);
}

TEST_CASE("HTTP decoder failures retain framing stage", "[http][http_client_result][framing]") {
    client_result<response> result;
    SECTION("malformed headers") {
        result = run_wire_response("HTTP/1.1 200 OK\r\nBroken header\r\n\r\n");
    }
    SECTION("truncated fixed-length body") {
        result = run_wire_response("HTTP/1.1 200 OK\r\nContent-Length: 9\r\nConnection: close\r\n\r\nx");
    }
    require_failure(result, EBADMSG, client_stage::framing);
}

TEST_CASE("HTTP body limit error is distinct from framing failure", "[http][http_client_result]") {
    const auto result = run_wire_response(
        "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nbody",
        [](elio::http::client_config& config) { config.max_response_size = 3; });
    require_failure(result, EMSGSIZE, client_stage::body);
}

TEST_CASE("HTTP write failures preserve request stage", "[http][http_client_result][write]") {
    write_failure_guard hook;
    const auto result = run_wire_response("");
    require_failure(result, EPIPE, client_stage::request);
}

TEST_CASE("HTTP invalid TLS peer reports TLS stage", "[http][http_client_result][tls]") {
    const auto result = run_wire_response("HTTP/1.1 200 OK\r\n\r\n", {}, true);
    REQUIRE(std::holds_alternative<client_error>(result));
    const auto error = std::get<client_error>(result);
    REQUIRE(error.code.value() > 0);
    REQUIRE(error.stage == client_stage::tls);
}

TEST_CASE("HTTP value API does not swallow exceptional failures", "[http][http_client_result][exception]") {
    write_failure_guard hook(throw_request_write);
    std::exception_ptr failure;
    (void)run_wire_response("", {}, false, &failure);
    REQUIRE(failure);
    REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::bad_alloc);
}

TEST_CASE("HTTP response cancellation reports the active read stage", "[http][http_client_result][cancel]") {
    SECTION("headers") { require_failure(run_stalled_response(false, true), ECANCELED, client_stage::headers); }
    SECTION("body") { require_failure(run_stalled_response(true, true), ECANCELED, client_stage::body); }
}

TEST_CASE("HTTP response deadline reports the active read stage", "[http][http_client_result][timeout]") {
    SECTION("headers") { require_failure(run_stalled_response(false, false), ETIMEDOUT, client_stage::headers); }
    SECTION("body") { require_failure(run_stalled_response(true, false), ETIMEDOUT, client_stage::body); }
}
