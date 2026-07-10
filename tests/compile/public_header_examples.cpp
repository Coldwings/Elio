#include <elio/elio.hpp>
#include <elio/http/http.hpp>
#include <elio/http/websocket_server.hpp>
#include <elio/tls/tls.hpp>

#include <iostream>
#include <string_view>
#include <tuple>
#include <utility>

using namespace elio;

namespace public_header_examples {

coro::task<int> compute() {
    co_return 42;
}

coro::task<int> async_main(int, char**) {
    int result = co_await compute();
    std::cout << "Result: " << result << std::endl;
    co_return 0;
}

coro::task<http::response> hello_handler(http::context&) {
    co_return http::response::ok("Hello, World!");
}

coro::task<void> http_server() {
    http::router r;
    r.get("/", hello_handler);
    r.get("/users/:id", [](http::context& ctx)
            -> coro::task<http::response> {
        auto id = ctx.param("id");
        co_return http::response::json(
            "{\"id\": \"" + std::string(id) + "\"}");
    });

    http::server srv(std::move(r));
    co_await elio::serve(srv, [&]() {
        return srv.listen(net::ipv4_address(8080));
    });
}

coro::task<void> http_client() {
    http::client c;
    auto resp = co_await c.get("https://example.com/");
    if (resp) {
        std::cout << "Status: " << resp->status_code() << std::endl;
        std::cout << "Body: " << resp->body() << std::endl;
    }

    auto post_resp = co_await c.post(
        "https://api.example.com/users",
        R"({"name": "John", "email": "john@example.com"})",
        http::mime::application_json);
    (void)post_resp;
}

coro::task<void> tls_client() {
    auto tls_ctx = tls::tls_context::make_client();
    auto stream = co_await tls::tls_connect(tls_ctx, "example.com", 443);
    if (!stream) co_return;

    co_await stream->write_exactly(
        "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n");

    char buffer[4096];
    auto result = co_await stream->read(buffer, sizeof(buffer));
    if (result.result > 0) {
        std::cout << std::string_view(buffer, result.result) << std::endl;
    }
    co_await stream->shutdown();
}

coro::task<void> tls_server() {
    auto tls_ctx = tls::tls_context::make_server("server.crt", "server.key");
    auto listener = tls::tls_listener::bind(net::ipv6_address(8443), tls_ctx);
    if (!listener) co_return;

    auto stream = co_await listener->accept();
    (void)stream;
}

coro::task<void> multiple_servers() {
    http::router http_router;
    http::websocket::ws_router ws_router;
    http::server http_srv(std::move(http_router));
    http::websocket::ws_server ws_srv(std::move(ws_router));
    auto addr1 = net::ipv4_address(8080);
    auto addr2 = net::ipv4_address(8081);

    co_await serve_all(
        std::tie(http_srv, ws_srv),
        std::make_tuple(
            [&]() { return http_srv.listen(addr1); },
            [&]() { return ws_srv.listen(addr2); }));
}

} // namespace public_header_examples

ELIO_ASYNC_MAIN(public_header_examples::async_main)
