#include <catch2/catch_test_macros.hpp>
#include <elio/elio.hpp>
#include <elio/http/http_server.hpp>
#include <elio/sync/event.hpp>

#include <future>
#include <thread>

using namespace elio;
using namespace elio::http;

namespace {
template<typename T>
T run_http_task(coro::task<T> operation) {
    auto handle = coro::detail::task_access::handle(operation);
    handle.resume();
    REQUIRE(handle.done());
    return operation.await_resume();
}

struct reply_sink {
    std::string wire;
    coro::task<io::io_result> writev(iovec* buffers, size_t count, coro::cancel_token) {
        size_t size = 0;
        for (size_t i = 0; i < count; ++i) {
            wire.append(static_cast<const char*>(buffers[i].iov_base), buffers[i].iov_len);
            size += buffers[i].iov_len;
        }
        co_return io::io_result{static_cast<int32_t>(size), 0};
    }
};

streaming_response context_stream(context& ctx, bool& invoked) {
    return streaming_response(status::ok,
        [&ctx, &invoked, owned = std::make_unique<int>(42)](body_writer& writer, coro::cancel_token) -> coro::task<send_result> {
            invoked = *owned == 42;
            co_return co_await writer.write(ctx.req().path());
        });
}

bool wait_http_condition(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!condition() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    return condition();
}

struct streaming_server_fixture {
    server service;
    runtime::scheduler scheduler{2};
    uint16_t port = 0;

    explicit streaming_server_fixture(router routes, server_config config = {})
        : service(std::move(routes), config) {
        auto reservation = net::tcp_listener::bind(net::ipv4_address("127.0.0.1", 0));
        REQUIRE(reservation);
        port = reservation->local_address().port();
        reservation.reset();
        scheduler.start();
        scheduler.go([this]() -> coro::task<void> {
            co_await service.listen(net::ipv4_address("127.0.0.1", port));
        });
        REQUIRE(wait_http_condition([this] { return service.is_running(); }));
    }
    ~streaming_server_fixture() {
        service.stop();
        (void)scheduler.shutdown(std::chrono::seconds(5));
    }

    std::future<std::string> request(std::string bytes) {
        auto result = std::make_shared<std::promise<std::string>>();
        auto future = result->get_future();
        scheduler.go([this, bytes = std::move(bytes), result]() -> coro::task<void> {
            try {
                auto connected = co_await net::tcp_connect(net::ipv4_address("127.0.0.1", port));
                if (!connected) {
                    result->set_value("connect failed");
                    co_return;
                }
                size_t offset = 0;
                while (offset < bytes.size()) {
                    auto sent = co_await connected->write(bytes.data() + offset, bytes.size() - offset);
                    if (sent.result <= 0) {
                        result->set_value("request write failed");
                        co_return;
                    }
                    offset += static_cast<size_t>(sent.result);
                }
                std::string received;
                char buffer[1024];
                for (;;) {
                    auto read = co_await connected->read(buffer, sizeof(buffer));
                    if (read.result <= 0) break;
                    received.append(buffer, static_cast<size_t>(read.result));
                }
                result->set_value(std::move(received));
            } catch (...) {
                result->set_exception(std::current_exception());
            }
        });
        return future;
    }
};
}

TEST_CASE("HTTP router normalizes six exact reply shapes", "[http][server][streaming]") {
    router routes;
    bool invoked = false;
    routes.get("/response", [](context&) { return response::ok("complete"); });
    routes.post("/stream", [&](context& ctx) { return context_stream(ctx, invoked); });
    routes.put("/reply", [](context&) -> reply { return response::ok("complete"); });
    routes.del("/task-response", [](context&) -> coro::task<response> { co_return response::ok("complete"); });
    routes.patch("/task-stream", [&](context& ctx) -> coro::task<streaming_response> {
        co_return context_stream(ctx, invoked);
    });
    routes.options("/task-reply", [](context&) -> coro::task<reply> { co_return response::ok("complete"); });
    const std::pair<method, std::string_view> cases[] = {
        {method::GET, "/response"}, {method::POST, "/stream"}, {method::PUT, "/reply"},
        {method::DELETE_, "/task-response"}, {method::PATCH, "/task-stream"}, {method::OPTIONS, "/task-reply"},
    };
    for (const auto& [verb, path] : cases) {
        std::unordered_map<std::string, std::string> params;
        const auto* route = routes.find_route(verb, path, params);
        REQUIRE(route);
        context ctx(http::request(verb, path), "fixture");
        auto selected = run_http_task(route->handler(ctx));
        http::detail::context_access::seal_final_response(ctx);
        reply_sink sink;
        invoked = false;
        REQUIRE(run_http_task(http::send_response(sink, selected, verb, "HTTP/1.1", false)).success());
        const bool streaming = path == "/stream" || path == "/task-stream";
        REQUIRE(invoked == streaming);
        REQUIRE(sink.wire.find(streaming ? std::string(path) : "complete") != std::string::npos);
    }
    static_assert(!http::detail::response_handler<decltype([](context&) { return 42; })>);
}

TEST_CASE("HTTP context survives handler selection but interims are sealed", "[http][server][streaming]") {
    coro::cancel_source source;
    size_t interim_calls = 0;
    context ctx(http::request(method::GET, "/borrowed-context"), "fixture",
        [&](std::string_view) -> coro::task<bool> { ++interim_calls; co_return true; }, source.get_token());
    REQUIRE(run_http_task(ctx.send_interim(response(status::early_hints))));
    REQUIRE(interim_calls == 1);
    bool context_alive = false;
    bool interim_blocked = false;
    reply selected{streaming_response(status::ok,
        [&](body_writer& writer, coro::cancel_token) -> coro::task<send_result> {
            context_alive = ctx.req().path() == "/borrowed-context";
            interim_blocked = !(co_await ctx.send_interim(response(status::early_hints))) && errno == EALREADY;
            co_return co_await writer.write(ctx.req().path());
        })};
    http::detail::context_access::seal_final_response(ctx);
    reply_sink sink;
    REQUIRE(run_http_task(http::send_response(sink, selected, method::GET, "HTTP/1.1", false)).success());
    REQUIRE(context_alive);
    REQUIRE(interim_blocked);
    REQUIRE(interim_calls == 1);
    REQUIRE_FALSE(ctx.cancel_token().is_cancelled());
    source.cancel();
    REQUIRE(ctx.cancel_token().is_cancelled());
}

TEST_CASE("HTTP server emits streamed wire and preserves context through producer", "[http][server][streaming]") {
    router routes;
    std::atomic<bool> context_alive{false};
    std::atomic<bool> sealed{false};
    routes.get("/hello/:name", [&](context& ctx) {
        return streaming_response(status::ok,
            [&ctx, &context_alive, &sealed](body_writer& writer, coro::cancel_token) -> coro::task<send_result> {
                context_alive = ctx.param("name") == "world";
                sealed = !(co_await ctx.send_interim(response(status::early_hints))) && errno == EALREADY;
                co_return co_await writer.write(ctx.param("name"));
            });
    });
    streaming_server_fixture fixture(std::move(routes));
    auto future = fixture.request("GET /hello/world HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto wire = future.get();
    REQUIRE(wire.starts_with("HTTP/1.1 200 OK\r\n"));
    REQUIRE(wire.find("Transfer-Encoding: chunked\r\n") != std::string::npos);
    REQUIRE(wire.ends_with("\r\n\r\n5\r\nworld\r\n0\r\n\r\n"));
    REQUIRE(context_alive);
    REQUIRE(sealed);
    REQUIRE(wait_http_condition([&] { return fixture.service.active_connections() == 0; }));
}

TEST_CASE("HTTP server HEAD skips a configured producer", "[http][server][streaming]") {
    router routes;
    std::atomic<bool> invoked{false};
    routes.add_route(method::HEAD, "/head", [&](context&) {
        return streaming_response(status::ok,
            [&](body_writer& writer, coro::cancel_token) -> coro::task<send_result> {
                invoked = true;
                co_return co_await writer.write("hello");
            }, 5);
    });
    streaming_server_fixture fixture(std::move(routes));
    auto future = fixture.request("HEAD /head HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto wire = future.get();
    REQUIRE(wire.find("Content-Length: 5\r\n") != std::string::npos);
    REQUIRE(wire.ends_with("\r\n\r\n"));
    REQUIRE(wire.find("hello") == std::string::npos);
    REQUIRE_FALSE(invoked);
}

TEST_CASE("HTTP server stop cancels producer session and drains active count", "[http][server][streaming]") {
    router routes;
    sync::event parked;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::atomic<bool> cancelled{false};
    routes.get("/park", [&](context& ctx) {
        return streaming_response(status::ok,
            [&ctx, &parked, &entered, &cancelled](body_writer&, coro::cancel_token token) -> coro::task<send_result> {
                entered.set_value();
                (void)co_await parked.wait(token);
                cancelled = token.is_cancelled() && ctx.cancel_token().is_cancelled();
                co_return send_result{send_errc::cancelled, ECANCELED};
            });
    });
    streaming_server_fixture fixture(std::move(routes));
    auto future = fixture.request("GET /park HTTP/1.1\r\nHost: localhost\r\n\r\n");
    REQUIRE(entered_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(fixture.service.active_connections() == 1);
    fixture.service.stop();
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto wire = future.get();
    REQUIRE(cancelled);
    REQUIRE(wire.find("Transfer-Encoding: chunked\r\n") != std::string::npos);
    REQUIRE_FALSE(wire.ends_with("0\r\n\r\n"));
    REQUIRE(wait_http_condition([&] { return fixture.service.active_connections() == 0; }));
}

TEST_CASE("HTTP not found handlers support the same six reply shapes", "[http][server][streaming]") {
    server service(router{});
    bool invoked = false;
    service.set_not_found_handler([](context&) { return response::not_found(); });
    service.set_not_found_handler([&](context& ctx) { return context_stream(ctx, invoked); });
    service.set_not_found_handler([](context&) -> reply { return response::not_found(); });
    service.set_not_found_handler([](context&) -> coro::task<response> { co_return response::not_found(); });
    service.set_not_found_handler([&](context& ctx) -> coro::task<streaming_response> { co_return context_stream(ctx, invoked); });
    service.set_not_found_handler([](context&) -> coro::task<reply> { co_return response::not_found(); });

    streaming_server_fixture fixture(router{});
    fixture.service.set_not_found_handler([](context&) {
        return streaming_response(status::not_found,
            [](body_writer& writer, coro::cancel_token) -> coro::task<send_result> {
                co_return co_await writer.write("missing");
            });
    });
    auto future = fixture.request("GET /missing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto wire = future.get();
    REQUIRE(wire.starts_with("HTTP/1.1 404 Not Found\r\n"));
    REQUIRE(wire.ends_with("7\r\nmissing\r\n0\r\n\r\n"));
}

TEST_CASE("HTTP server producer failure closes without a successful terminator", "[http][server][streaming]") {
    router routes;
    routes.get("/failure", [](context&) {
        return streaming_response(status::ok,
            [](body_writer& writer, coro::cancel_token) -> coro::task<send_result> {
                auto result = co_await writer.write("abc");
                if (!result.success()) co_return result;
                throw std::runtime_error("producer failed after progress");
            });
    });
    streaming_server_fixture fixture(std::move(routes));
    auto future = fixture.request("GET /failure HTTP/1.1\r\nHost: localhost\r\n\r\n");
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto wire = future.get();
    REQUIRE(wire.ends_with("3\r\nabc\r\n"));
    REQUIRE_FALSE(wire.ends_with("0\r\n\r\n"));
    REQUIRE(wait_http_condition([&] { return fixture.service.active_connections() == 0; }));
}

TEST_CASE("HTTP server stop cancels an accepted idle request read", "[http][server][streaming]") {
    server_config config;
    config.keep_alive_timeout = std::chrono::seconds(0);
    streaming_server_fixture fixture(router{}, config);
    auto future = fixture.request("");
    REQUIRE(wait_http_condition([&] { return fixture.service.active_connections() == 1; }));
    fixture.service.stop();
    REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(future.get().empty());
    REQUIRE(wait_http_condition([&] { return fixture.service.active_connections() == 0; }));
}
