#include <elio/elio.hpp>
#include <elio/http/http_client.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>

elio::coro::task<int> async_main(int, char**) {
    using namespace elio;
    http::transport_config config;
    config.limits = http::pool_limits{};
    config.limits->max_live_total = 4;
    config.acquisition_timeout = std::chrono::seconds(5);
    auto owner = std::make_shared<http::transport>(config);
    http::client client(owner);
    auto invalid = co_await client.get_result("unsupported://host/");
    const auto* saved = std::get_if<http::client_error>(&invalid);
    if (!saved) co_return 1;

    auto listener = net::tcp_listener::bind(net::ipv4_address("127.0.0.1", 0));
    if (!listener) co_return 1;
    coro::cancel_source stop;
    auto server = runtime::scheduler::current()->go_joinable([&]() -> coro::task<void> {
        auto peer = co_await listener->accept(stop.get_token());
        if (!peer) co_return;
        std::array<char, 1024> buffer{};
        std::string headers;
        while (headers.find("\r\n\r\n") == std::string::npos && headers.size() < 16384) {
            auto read = co_await peer->read(buffer.data(), buffer.size(), stop.get_token());
            if (read.result <= 0) co_return;
            headers.append(buffer.data(), static_cast<size_t>(read.result));
        }
        (void)co_await peer->write_exactly(
            "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
            stop.get_token());
    });
    const auto target = "http://127.0.0.1:" + std::to_string(listener->local_address().port()) + "/";
    auto result = co_await client.get_result(target);
    stop.cancel();
    co_await server;
    const auto* response = std::get_if<http::response>(&result);
    if (!response || response->status_code() != 503) co_return 1;
    std::cout << "HTTP status: " << response->status_code()
              << "; owned error: " << saved->code.value() << " at target\n";
    co_return saved->stage == http::client_stage::target && saved->code.value() == EINVAL ? 0 : 1;
}

ELIO_ASYNC_MAIN(async_main)
