#pragma once

#include <elio/http/http_reply.hpp>
#include <elio/http/http_body_writer.hpp>

#include <cerrno>
#include <chrono>
#include <new>

namespace elio::http {

struct response_send_result {
    send_result result;
    bool reusable = false;

    bool success() const noexcept { return result.success(); }
};

/// Execute one final reply. The caller owns the connection and must close it
/// after failure/nonreuse. Keep reply and stream alive and unmoved through
/// this await. No body bytes are copied into a serialization buffer.
template<typename Stream>
coro::task<response_send_result> send_response(
    Stream& stream, reply& selected, method request_method,
    std::string_view request_version, bool allow_reuse,
    coro::cancel_token token = {}, std::chrono::nanoseconds write_timeout = {}) {
    response_head* head;
    body_description description;
    std::string_view complete_body;
    auto* complete = std::get_if<response>(&selected);
    auto* streaming = std::get_if<streaming_response>(&selected);
    if (complete) {
        head = complete;
        complete_body = complete->body();
        description = {response_body_kind::complete, complete_body.size(),
                       response_transfer::automatic};
    } else if (streaming) {
        if (!detail::response_sender_access::can_produce(*streaming)) {
            co_return response_send_result{{send_errc::invalid_state}};
        }
        head = streaming;
        description = {response_body_kind::streaming, streaming->body_length(),
                       streaming->transfer()};
    } else {
        co_return response_send_result{{send_errc::invalid_response, EINVAL}};
    }

    response_plan plan;
    try {
        plan = prepare_response(*head, description, request_method,
                                request_version, allow_reuse);
    } catch (const std::bad_alloc&) {
        co_return response_send_result{{send_errc::invalid_response, ENOMEM}};
    } catch (...) {
        co_return response_send_result{{send_errc::invalid_response, EINVAL}};
    }
    if (!plan.success()) {
        co_return response_send_result{{send_errc::invalid_response, plan.error}};
    }

    auto writer = detail::body_writer_access::create(stream, plan, token, write_timeout);
    try {
        auto sent = co_await detail::body_writer_access::send_headers(
            writer, plan.header_block);
        if (!sent.success()) co_return response_send_result{sent};

        if (plan.invoke_producer) {
            send_result produced;
            try {
                produced = co_await detail::response_sender_access::produce(
                    *streaming, writer, token);
            } catch (...) {
                produced = {send_errc::producer_error};
            }
            if (!produced.success()) {
                co_return response_send_result{
                    detail::body_writer_access::fail(writer, produced)};
            }
        } else if (complete && plan.framing != response_framing::none) {
            sent = co_await writer.write(complete_body);
            if (!sent.success()) co_return response_send_result{sent};
        }

        sent = co_await detail::body_writer_access::finish(writer);
        co_return response_send_result{sent, sent.success() && plan.reusable};
    } catch (const std::bad_alloc&) {
        co_return response_send_result{detail::body_writer_access::fail(
            writer, {send_errc::transport_error, ENOMEM})};
    } catch (...) {
        co_return response_send_result{detail::body_writer_access::fail(
            writer, {send_errc::transport_error, EIO})};
    }
}

} // namespace elio::http
