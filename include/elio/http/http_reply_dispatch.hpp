#pragma once

#include <elio/http/http_response_sender.hpp>
#include <elio/http/http_parser.hpp>
#include <elio/http/http_tunnel.hpp>

namespace elio::http::detail {

struct reply_dispatch_result {
    response_send_result ordinary;
    std::optional<tunnel_result> tunnel;
    bool reusable = false;
    bool success() const noexcept { return tunnel ? tunnel->success() : ordinary.success(); }
};

/// Server-owned handoff: parser, transport and selected reply outlive this await.
/// A tunnel always ends HTTP processing, including failed preflight/acceptance.
template<class Stream>
coro::task<reply_dispatch_result> dispatch_reply(
    Stream& stream, reply& selected, request_parser& parser,
    bool allow_reuse, coro::cancel_token token = {},
    std::chrono::nanoseconds write_timeout = {}) {
    auto* accepted = std::get_if<tunnel_response>(&selected);
    if (!accepted) {
        auto sent = co_await send_response(stream, selected, parser.get_method(),
            parser.version(), allow_reuse && parser.get_method() != method::CONNECT,
            token, write_timeout);
        co_return reply_dispatch_result{sent, std::nullopt, sent.success() && sent.reusable};
    }
    const auto invalid = [] {
        return reply_dispatch_result{{{send_errc::invalid_response, EINVAL}, false},
                                     std::nullopt, false};
    };
    if (!parser.is_complete() || parser.get_method() != method::CONNECT ||
        (parser.version() != "HTTP/1.0" && parser.version() != "HTTP/1.1") ||
        accepted->status_code() < 200 || accepted->status_code() >= 300 ||
        accepted->get_headers().contains("Content-Length") ||
        accepted->get_headers().contains("Transfer-Encoding") ||
        !tunnel_response_access::can_run(*accepted)) co_return invalid();

    try {
        auto version = accepted->version();
        if (version.empty()) version = parser.version();
        if (version != "HTTP/1.0" && version != "HTTP/1.1") co_return invalid();
        if (parser.version() == "HTTP/1.0") version = "HTTP/1.0";
        const auto headers = std::string(version) + ' ' +
            std::to_string(accepted->status_code()) + ' ' +
            std::string(status_reason(accepted->get_status())) + "\r\n" +
            accepted->get_headers().serialize() + "\r\n";
        // Use only the joined logical header-write machinery. No ordinary
        // CONNECT response plan, body producer, or HTTP final marker is run.
        response_plan plan;
        auto writer = body_writer_access::create(stream, plan, token, write_timeout);
        auto sent = co_await body_writer_access::send_headers(writer, headers);
        if (!sent.success())
            co_return reply_dispatch_result{{sent, false}, std::nullopt, false};
        if (token.is_cancelled())
            co_return reply_dispatch_result{{{send_errc::cancelled, ECANCELED}, false}, std::nullopt, false};
        auto tunnel = tunnel_stream_access::create(stream, parser.take_remaining());
        auto result = co_await tunnel_response_access::run(*accepted, tunnel, token);
        co_return reply_dispatch_result{{}, std::move(result), false};
    } catch (const std::bad_alloc&) {
        co_return reply_dispatch_result{{{send_errc::transport_error, ENOMEM}, false}, std::nullopt, false};
    } catch (...) {
        co_return reply_dispatch_result{{}, tunnel_result{tunnel_end::callback_error, EIO}, false};
    }
}

} // namespace elio::http::detail
