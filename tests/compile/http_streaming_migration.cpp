// Current-side counterparts of wiki/Migrating-to-0.6.md's HTTP examples.
// Historical removed APIs deliberately do not appear in this translation unit.
#include <elio/http/http_server.hpp>
#include <elio/http/sse_writer.hpp>

#include <array>
#include <cerrno>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace http_streaming_migration {
using namespace elio;
using namespace elio::http;

response changed_body() {
    auto value = response::ok("old");
    value.set_header("Content-Length", "3");
    value.set_body(std::string_view("longer"));
    value.get_headers().remove("Content-Length");
    return value;
}

bool stale_length_is_rejected() {
    auto value = response::ok("old");
    value.set_header("Content-Length", "3");
    value.set_body(std::string_view("longer"));
    try {
        (void)value.serialize();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

std::string serialize_decoded_body(response received) {
    received.get_headers().remove("Transfer-Encoding");
    received.get_headers().remove("Content-Length");
    return received.serialize();
}

streaming_response close_delimited_reply() {
    return streaming_response(status::ok,
        [data = std::string("hello")](body_writer& writer,
            coro::cancel_token token) -> coro::task<send_result> {
            co_return co_await writer.write(data, token);
        }, std::nullopt, response_transfer::close_delimited);
}

streaming_response chunked_reply() {
    return streaming_response(status::ok,
        [data = std::string("hello")](body_writer& writer,
            coro::cancel_token token) -> coro::task<send_result> {
            co_return co_await writer.write(data, token);
        });
}

void register_migrated_routes(router& routes) {
    routes.get("/ordinary", [](context&) -> coro::task<response> {
        co_return response::ok("ordinary");
    });
    handler_func selected = [](context&) -> coro::task<reply> {
        co_return chunked_reply();
    };
    routes.get("/stream", std::move(selected));
    routes.get("/events", [](context&) {
        return sse::make_streaming_response(
            [](sse::event_writer& writer, coro::cancel_token token) -> coro::task<send_result> {
                const auto sent = co_await writer.send_data("hello", token);
                if (!sent.success()) co_return sent;
                co_return send_result{};
            });
    });
}

coro::task<send_result> migrated_sse_id(sse::event_writer& writer,
                                      std::string_view id, coro::cancel_token token) {
    const auto sent = co_await writer.send_event(
        {id.empty() ? std::nullopt : std::optional<std::string_view>{id}, {}, "data"},
        token);
    if (!sent.success()) co_return sent;
    co_return co_await writer.send_event({std::string_view{}, {}, "reset"}, token);
}

coro::task<send_result> borrowed_write(body_writer& writer, coro::cancel_token token) {
    std::string first = "hello ";
    std::string second = "world";
    const std::array<body_buffer, 2> parts{{
        {first.data(), first.size()}, {second.data(), second.size()}}};
    const auto sent = co_await writer.writev(std::span<const body_buffer>(parts), token);
    if (!sent.success()) co_return sent;
    first.assign("safe to reuse after the awaited operation and cleanup");
    co_return sent;
}

streaming_response failing_source() {
    return streaming_response(status::ok,
        [](body_writer& writer, coro::cancel_token token) -> coro::task<send_result> {
            const auto sent = co_await writer.write("prefix", token);
            if (!sent.success()) co_return sent;
            // Simulates a source error discovered after final headers.
            co_return send_result{send_errc::producer_error, EIO};
        });
}

// Optional runtime entry point for a focused harness; compilation alone does
// not establish these framing assertions. No scheduler or transport required.
bool metadata_migration_examples() {
    const auto changed = changed_body();
    if (changed.get_headers().contains("Content-Length") ||
        changed.serialize().find("Content-Length: 6\r\n") == std::string::npos ||
        !stale_length_is_rejected()) return false;

    response_parser parser;
    const auto parsed = parser.parse(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    if (parsed.first != parse_result::complete) return false;
    auto received = response::from_parser(parser);
    if (received.header("Transfer-Encoding") != "chunked" || received.body() != "hello") return false;
    try {
        (void)received.serialize();
        return false;
    } catch (const std::invalid_argument&) {
    }
    const auto wire = serialize_decoded_body(received);
    return received.header("Transfer-Encoding") == "chunked" &&
        wire.find("Transfer-Encoding:") == std::string::npos &&
        wire.find("Content-Length: 5\r\n") != std::string::npos && wire.ends_with("\r\n\r\nhello");
}

} // namespace http_streaming_migration
