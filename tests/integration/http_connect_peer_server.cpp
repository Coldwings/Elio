// Restricted loopback CONNECT interoperability fixture; not a public proxy.
#include <elio/elio.hpp>
#include <elio/http/http.hpp>
#include <elio/http/websocket_server.hpp>
#include <charconv>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

using namespace elio;
using namespace elio::http;
using namespace std::chrono_literals;

template<class Service>
int serve_peer(Service& service, tls::tls_context* security) {
    auto reservation = net::tcp_listener::bind(net::ipv4_address("127.0.0.1", 0));
    if (!reservation) return 2;
    const auto port = reservation->local_address().port();
    reservation.reset(); // A bind race is an explicit startup failure.
    runtime::scheduler scheduler(2);
    std::promise<void> done;
    auto future = done.get_future();
    scheduler.start();
    scheduler.go([&]() -> coro::task<void> {
        try {
            if (security) co_await service.listen_tls(net::ipv4_address("127.0.0.1", port), *security);
            else co_await service.listen(net::ipv4_address("127.0.0.1", port));
            done.set_value();
        } catch (...) { done.set_exception(std::current_exception()); }
    });
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!service.is_running() && std::chrono::steady_clock::now() < deadline &&
           future.wait_for(0s) != std::future_status::ready) std::this_thread::yield();
    int result = service.is_running() ? 0 : 3;
    if (!result) {
        std::cout << "READY " << port << std::endl;
        std::string command;
        std::getline(std::cin, command);
        if (command != "stop") result = 4;
    }
    service.stop();
    if (future.wait_for(5s) != std::future_status::ready) result = 5;
    else { try { future.get(); } catch (...) { result = 6; } }
    if (!scheduler.shutdown(5s)) {
        std::cerr << "scheduler failed to join; preserving borrowed service lifetime by terminating\n";
        std::_Exit(7);
    }
    if (service.active_connections()) result = 8;
    if (!result) std::cout << "STOPPED" << std::endl;
    return result;
}

int main(int argc, char** argv) {
    try {
        log::logger::instance().set_level(log::level::error);
        std::string frontend, transport, certificate, key;
        uint16_t upstream_port = 0;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 == argc) throw std::invalid_argument("missing value");
            const std::string_view name(argv[i]), value(argv[i + 1]);
            if (name == "--frontend") frontend = value;
            else if (name == "--transport") transport = value;
            else if (name == "--cert") certificate = value;
            else if (name == "--key") key = value;
            else if (name == "--upstream-port") {
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), upstream_port);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                    throw std::invalid_argument("invalid port");
            } else throw std::invalid_argument("unknown option");
        }
        if (!upstream_port || (frontend != "http" && frontend != "websocket") ||
            (transport != "tcp" && transport != "tls12" && transport != "tls13"))
            throw std::invalid_argument("invalid coordinate");
        websocket::ws_router routes;
        routes.get("/ordinary", [](context&) { return response::ok("ordinary"); });
        routes.add_route(method::CONNECT, "*",
            [](context&) { return response::internal_error("path routing was used"); });
        routes.websocket("*", [](websocket::ws_connection&) -> coro::task<void> { co_return; });
        routes.connect([upstream_port](context& ctx, connect_authority_view authority) -> coro::task<reply> {
            if (authority.host != "127.0.0.1" || authority.port != upstream_port)
                co_return response(status::forbidden, "denied");
            auto upstream = co_await net::tcp_connect(net::ipv4_address("127.0.0.1", upstream_port), ctx.cancel_token());
            if (!upstream) co_return response(status::bad_gateway, "upstream failed");
            tunnel_response selected([upstream = net::stream(std::move(*upstream))]
                (tunnel_stream& client, coro::cancel_token token) mutable -> coro::task<tunnel_result> {
                    co_return co_await relay(client, upstream, {}, token);
                });
            selected.set_header("X-Authority", authority.raw);
            co_return reply{std::move(selected)};
        });
        std::unique_ptr<tls::tls_context> security;
        if (transport != "tcp") {
            security = std::make_unique<tls::tls_context>(tls::tls_mode::server,
                transport == "tls12" ? tls::tls_version::tls_1_2 : tls::tls_version::tls_1_3);
            if (!security->load_certificate(certificate) || !security->load_private_key(key))
                throw std::runtime_error("certificate loading failed");
        }
        server_config config;
        config.enable_logging = false;
        config.write_timeout = 5s;
        config.keep_alive_timeout = 5s;
        if (frontend == "websocket") {
            websocket::ws_server service(std::move(routes), config);
            return serve_peer(service, security.get());
        }
        server service(std::move(routes), config);
        return serve_peer(service, security.get());
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
