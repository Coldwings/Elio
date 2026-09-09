// An intentionally restricted example, not an open forward proxy.
// Usage: http_connect_proxy --port 8080 --upstream-port 9000
#include <elio/elio.hpp>
#include <elio/http/http.hpp>
#include <elio/http/http_tunnel_relay.hpp>

#include <charconv>
#include <cstdio>
#include <stdexcept>
#include <string_view>

using namespace elio;
using namespace elio::http;

namespace {
router proxy_routes(uint16_t upstream_port) {
    router routes;
    routes.connect([upstream_port](context& ctx, connect_authority_view authority)
        -> coro::task<reply> {
        // No DNS resolution or request-selected network destination. A real
        // deployment adds authentication and its own destination policy here.
        if ((authority.host != "127.0.0.1" && authority.host != "localhost") ||
            authority.port != upstream_port) {
            co_return response(status::forbidden, "Destination is not allowed");
        }
        auto connected = co_await net::tcp_connect(
            net::ipv4_address("127.0.0.1", upstream_port), ctx.cancel_token());
        if (!connected) co_return response(status::bad_gateway, "Upstream connection failed");
        co_return tunnel_response(status::ok,
            [upstream = net::stream(std::move(*connected))]
            (tunnel_stream& client, coro::cancel_token token) mutable -> coro::task<tunnel_result> {
                // The callback owns upstream through both relay directions and
                // their cleanup. Inner TLS, if any, is opaque TCP payload here.
                co_return co_await relay(client, upstream, {}, std::move(token));
            });
    });
    return routes;
}

coro::task<int> serve_proxy(uint16_t port, uint16_t upstream_port) {
    server_config config;
    config.write_timeout = std::chrono::seconds(5);
    server service(proxy_routes(upstream_port), config);
    const net::socket_address address = net::ipv4_address("127.0.0.1", port);
    ELIO_LOG_INFO("Restricted CONNECT proxy on 127.0.0.1:{}; allowed upstream 127.0.0.1:{}",
                 port, upstream_port);
    co_await elio::serve(service, [&] { return service.listen(address); });
    co_return 0;
}

uint16_t parse_port(std::string_view value) {
    uint16_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !result)
        throw std::invalid_argument("port must be in 1..65535");
    return result;
}
}

int main(int argc, char** argv) {
    try {
        uint16_t port = 0;
        uint16_t upstream_port = 0;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 == argc) throw std::invalid_argument("missing option value");
            const std::string_view name(argv[i]);
            if (name == "--port" && !port) port = parse_port(argv[i + 1]);
            else if (name == "--upstream-port" && !upstream_port) upstream_port = parse_port(argv[i + 1]);
            else throw std::invalid_argument("unknown or duplicate option");
        }
        if (!port || !upstream_port) throw std::invalid_argument("both ports are required");
        signal::signal_set shutdown_signals(default_shutdown_signals);
        shutdown_signals.block_all_threads();
        return elio::run(serve_proxy, port, upstream_port);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\nUsage: http_connect_proxy --port PORT --upstream-port PORT\n", error.what());
        return 1;
    }
}
