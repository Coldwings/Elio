#include <elio/elio.hpp>
#include <elio/http/http_client.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <span>
#include <string>

elio::coro::task<int> async_main(int, char**) {
    using namespace elio;
    constexpr size_t range_size = 65536;
    std::array<char, range_size> destination{};
    auto listener = net::tcp_listener::bind(net::ipv4_address("127.0.0.1", 0));
    if (!listener) co_return 1;
    coro::cancel_source stop;
    auto peer = runtime::scheduler::current()->go_joinable([&]() -> coro::task<void> {
        auto stream = co_await listener->accept(stop.get_token());
        if (!stream) co_return;
        std::array<char, 4096> chunk;
        std::string request_headers;
        while (request_headers.find("\r\n\r\n") == std::string::npos &&
               request_headers.size() < 16384) {
            auto read = co_await stream->read(chunk.data(), chunk.size(), stop.get_token());
            if (read.result <= 0) co_return;
            request_headers.append(chunk.data(), static_cast<size_t>(read.result));
        }
        auto head = co_await stream->write_exactly(
            "HTTP/1.1 206 Partial Content\r\nContent-Length: 65536\r\n"
            "Content-Range: bytes 0-65535/65536\r\nConnection: close\r\n\r\n", stop.get_token());
        if (head.result <= 0) co_return;
        chunk.fill('r');
        for (size_t sent = 0; sent < range_size; sent += chunk.size()) {
            auto written = co_await stream->write_exactly(chunk.data(), chunk.size(), stop.get_token());
            if (written.result <= 0) co_return;
        }
    });

    http::client_config config;
    config.max_response_size = 1024; // A buffered call could not accept this body.
    config.read_buffer_size = 4096;
    http::client client(config);
    auto target = *http::url::parse("http://127.0.0.1:" +
        std::to_string(listener->local_address().port()) + "/range");
    http::request request(http::method::GET, target.path_with_query());
    request.set_header("Range", "bytes=0-65535");
    http::streaming_response_options options;
    options.max_body_size = range_size;
    size_t received = 0;
    bool accepted = false;
    bool complete = false;
    http::client_result<std::monostate> result;
    std::exception_ptr failure;
    try {
        result = co_await client.with_response(std::move(request), std::move(target),
            stop.get_token(), [&](const http::response& head, http::response_body_reader& body,
                                   coro::cancel_token token) -> coro::task<void> {
                accepted = head.status_code() == 206 && head.body().empty() &&
                    head.header("Content-Range") == "bytes 0-65535/65536";
                if (!accepted) co_return; // Header rejection closes without draining.
                char extra;
                for (;;) {
                    // A final nonempty pull observes framing completion after
                    // the expected bytes, without mistaking an empty span for EOF.
                    auto remaining = std::span<char>(destination).subspan(received);
                    auto buffer = remaining.empty() ? std::span<char>(&extra, 1) : remaining;
                    auto read = co_await body.read_into(buffer, token);
                    if (std::holds_alternative<http::client_error>(read)) co_return;
                    auto progress = std::get<http::body_read_progress>(read);
                    if (progress.transferred > remaining.size()) co_return;
                    received += progress.transferred;
                    if (progress.complete) {
                        complete = true;
                        co_return;
                    }
                }
            }, options);
    } catch (...) {
        failure = std::current_exception();
    }
    stop.cancel();
    try { co_await peer; } catch (...) { if (!failure) failure = std::current_exception(); }
    co_await peer.wait_destroyed_async();
    if (failure) std::rethrow_exception(failure);
    if (const auto* error = std::get_if<http::client_error>(&result)) {
        std::cerr << "HTTP exchange failed: " << error->code.message() << '\n';
        co_return 1;
    }
    if (!accepted || !complete || received != destination.size() ||
        !std::all_of(destination.begin(), destination.end(), [](char c) { return c == 'r'; })) {
        co_return 1;
    }
    std::cout << "Streamed range: " << received << " bytes; response body not aggregated\n";
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
