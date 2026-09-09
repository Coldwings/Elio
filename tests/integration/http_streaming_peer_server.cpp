// Standalone loopback peer for http_streaming_peer.py. No benchmark claims.
#include <elio/elio.hpp>
#include <elio/http/http_server.hpp>
#include <elio/http/sse_writer.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <thread>

using namespace elio;
using namespace elio::http;

int main() {
    std::atomic<unsigned> head_invocations{0};
    router routes;
    routes.get("/ordinary", [](context&) { return response::ok("hello world"); });
    auto produce = [](body_writer& writer, coro::cancel_token token) -> coro::task<send_result> {
        auto result = co_await writer.write("hello ", token);
        if (!result.success()) co_return result;
        co_return co_await writer.write("world", token);
    };
    routes.get("/known", [produce](context&) {
        return streaming_response(status::ok, produce, 11);
    });
    routes.get("/chunked", [produce](context&) {
        return streaming_response(status::ok, produce);
    });
    routes.add_route(method::HEAD, "/head", [&](context&) {
        return streaming_response(status::ok,
            [&](body_writer& writer, coro::cancel_token token) -> coro::task<send_result> {
                ++head_invocations;
                co_return co_await writer.write("hello world", token);
            }, 11);
    });
    routes.get("/head-invocations", [&](context&) {
        return response::ok(std::to_string(head_invocations.load()));
    });
    routes.get("/failure", [](context&) {
        return streaming_response(status::ok,
            [](body_writer& writer, coro::cancel_token token) -> coro::task<send_result> {
                auto result = co_await writer.write("prefix", token);
                if (!result.success()) co_return result;
                co_return send_result{send_errc::producer_error};
            });
    });
    routes.get("/sse", [](context&) {
        return sse::make_streaming_response(
            [](sse::event_writer& writer, coro::cancel_token token) -> coro::task<send_result> {
                auto result = co_await writer.send_event({"7", "update", "alpha\r\nbeta\n"}, token);
                if (!result.success()) co_return result;
                co_return co_await writer.send_data("\xe4\xbd\xa0\xe5\xa5\xbd", token);
            });
    });

    // The current server API does not expose its ephemeral listening port.
    // Reserve one first; a bind race fails startup explicitly, never silently
    // substituting another server as the test peer.
    auto reservation = net::tcp_listener::bind(net::ipv4_address("127.0.0.1", 0));
    if (!reservation) return 2;
    const auto port = reservation->local_address().port();
    reservation.reset();
    server_config config;
    config.enable_logging = false;
    config.keep_alive_timeout = std::chrono::seconds(10);
    config.write_timeout = std::chrono::seconds(5);
    server service(std::move(routes), config);
    runtime::scheduler scheduler(2);
    std::promise<void> listener_done;
    auto listener_future = listener_done.get_future();
    scheduler.start();
    scheduler.go([&]() -> coro::task<void> {
        try {
            co_await service.listen(net::ipv4_address("127.0.0.1", port));
            listener_done.set_value();
        } catch (...) {
            listener_done.set_exception(std::current_exception());
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!service.is_running() && std::chrono::steady_clock::now() < deadline &&
           listener_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        std::this_thread::yield();
    }
    int result = service.is_running() ? 0 : 3;
    if (result == 0) {
        std::cout << "READY " << port << std::endl;
        // Blocking control input belongs to the main thread, not a worker.
        std::string command;
        std::getline(std::cin, command);
        if (command != "stop") result = 4;
    }
    service.stop();
    // Listener completion closes the accept-to-session registration window.
    if (listener_future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        result = 5;
    } else {
        try { listener_future.get(); }
        catch (...) { result = 6; }
    }
    if (!scheduler.shutdown(std::chrono::seconds(5))) result = 7;
    if (service.active_connections() != 0) result = 8;
    if (result == 0) std::cout << "STOPPED" << std::endl;
    return result;
}
