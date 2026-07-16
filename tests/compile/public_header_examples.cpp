#include <elio/elio.hpp>
#include <elio/http/http.hpp>
#include <elio/http/websocket.hpp>
#include <elio/rpc/rpc.hpp>
#include <elio/tls/tls.hpp>

#include <chrono>
#include <iostream>
#include <span>
#include <string_view>
#include <sys/uio.h>
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

namespace websocket_header_example {

using namespace elio;
using namespace elio::http::websocket;

coro::task<int> async_main(int, char**) {
    ws_router router;
    router.get("/", [](http::context&) -> coro::task<http::response> {
        co_return http::response::ok("Hello!");
    });
    co_return 0;
}

int run_entry_point(int argc, char* argv[]) {
    return elio::run(async_main, argc, argv);
}

} // namespace websocket_header_example

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

coro::task<void> rpc_client_with_resolve_options() {
    rpc::rpc_client_config cfg;
    net::resolve_options resolve_opts;
    auto client = co_await rpc::tcp_rpc_client::connect_with_config(
        cfg, "localhost", 9000, resolve_opts);
    (void)client;
}

void rpc_server_config_aggregate_compat() {
    rpc::rpc_server_config legacy_order{
        1024,
        std::chrono::seconds(30),
        rpc::max_message_size,
    };
    (void)legacy_order;
}

coro::task<void> uds_cancellable_api(coro::cancel_token token) {
    auto stream = co_await net::uds_connect(
        net::unix_address::abstract("public_header_uds_cancel"), token);
    auto path_stream = co_await net::uds_connect(
        std::string_view("/tmp/public_header_uds_cancel.sock"), token);
    (void)path_stream;

    if (!stream) co_return;

    char buffer[16]{};
    auto read_result = co_await stream->read(buffer, sizeof(buffer), token);
    (void)read_result;
    auto span_read_result = co_await stream->read(std::span<char>(buffer), token);
    (void)span_read_result;

    auto exact_result = co_await stream->read_exactly(
        buffer, sizeof(buffer), token);
    (void)exact_result;
    auto span_exact_result = co_await stream->read_exactly(
        std::span<char>(buffer), token);
    (void)span_exact_result;
}

coro::task<void> vectored_io(int fd) {
    char first[8]{};
    char second[8]{};
    struct iovec iovecs[2] = {
        {first, sizeof(first)},
        {second, sizeof(second)},
    };

    auto read_result = co_await io::async_readv(fd, iovecs, 2);
    (void)read_result;

    auto write_result = co_await io::async_writev(fd, iovecs, 2);
    (void)write_result;
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
