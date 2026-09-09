#pragma once

#include <elio/http/http_body_writer.hpp>
#include <elio/http/http_streaming_response.hpp>

#include <array>
#include <charconv>
#include <cerrno>
#include <functional>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace elio::http::sse {

namespace detail { struct event_writer_access; }

/// Borrowed event fields, valid and immutable through send_event completion.
/// Absent id is omitted; present empty id resets Last-Event-ID.
/// Empty type and negative retry are omitted. Data is always emitted.
struct event_view {
    std::optional<std::string_view> id;
    std::string_view type;
    std::string_view data;
    int retry = -1;
};

/// Sequential producer-scoped SSE encoder. Borrows event data, using fixed-size
/// descriptor batches rather than constructing an encoded event string. Do not
/// escape this object or overlap its operations. The first failure is sticky.
/// HTTP framing and response completion belong exclusively to body_writer/server.
class event_writer {
public:
    event_writer(const event_writer&) = delete;
    event_writer& operator=(const event_writer&) = delete;

    send_result result() const noexcept { return result_; }

    /// Reject CR/LF/NUL in id/type before emitting any bytes of this event.
    /// Unlike legacy serialize_event(), invalid field values are not omitted.
    coro::task<send_result> send_event(event_view value, coro::cancel_token token = {}) {
        try {
            return encode_event(value, std::move(token));
        } catch (const std::bad_alloc&) {
            latch_exception(ENOMEM);
            throw;
        } catch (...) {
            latch_exception(EIO);
            throw;
        }
    }

    coro::task<send_result> send_data(std::string_view data, coro::cancel_token token = {}) {
        return send_event({{}, {}, data}, std::move(token));
    }

    /// Each CR, LF or CRLF-delimited line gets its own comment prefix.
    coro::task<send_result> send_comment(std::string_view text, coro::cancel_token token = {}) {
        return emit(": ", text, {}, std::move(token));
    }

private:
    friend struct detail::event_writer_access;
    explicit event_writer(body_writer& writer) noexcept : writer_(writer) {}

    void latch_exception(int error) noexcept {
        if (result_.success()) {
            result_.error = send_errc::producer_error;
            result_.transport_error = error;
        }
    }

    coro::task<send_result> encode_event(event_view value, coro::cancel_token token) {
        if (!result_.success()) co_return result_;
        if ((value.id && invalid_field(*value.id)) || invalid_field(value.type)) {
            result_.error = send_errc::invalid_response;
            result_.transport_error = EINVAL;
            co_return result_;
        }
        std::array<body_buffer, 9> fields{};
        size_t count = 0;
        auto field = [&](std::string_view prefix, std::string_view text) {
            fields[count++] = {prefix.data(), prefix.size()};
            fields[count++] = {text.data(), text.size()};
            fields[count++] = {"\n", 1};
        };
        if (value.id) field(value.id->empty() ? "id:" : "id: ", *value.id);
        if (!value.type.empty()) field("event: ", value.type);
        std::array<char, 24> retry{};
        if (value.retry >= 0) {
            const auto converted = std::to_chars(retry.data(), retry.data() + retry.size(), value.retry);
            field("retry: ", {retry.data(), static_cast<size_t>(converted.ptr - retry.data())});
        }
        co_return co_await emit("data: ", value.data,
            std::span<const body_buffer>(fields.data(), count), std::move(token));
    }

    static bool invalid_field(std::string_view value) noexcept {
        return value.find_first_of(std::string_view("\r\n\0", 3)) != std::string_view::npos;
    }

    coro::task<send_result> emit(std::string_view prefix, std::string_view text,
                                std::span<const body_buffer> fields,
                                coro::cancel_token token) {
        try {
            return emit_lines(prefix, text, fields, std::move(token));
        } catch (const std::bad_alloc&) {
            latch_exception(ENOMEM);
            throw;
        } catch (...) {
            latch_exception(EIO);
            throw;
        }
    }

    coro::task<send_result> emit_lines(std::string_view prefix, std::string_view text,
                                      std::span<const body_buffer> fields,
                                      coro::cancel_token token) {
        if (!result_.success()) co_return result_;
        std::array<body_buffer, 24> batch{};
        size_t count = 0;
        for (const auto field : fields) batch[count++] = field;
        size_t start = 0;
        while (true) {
            if (count + 3 > batch.size()) {
                result_ = co_await writer_.writev(
                    std::span<const body_buffer>(batch.data(), count), token);
                if (!result_.success()) co_return result_;
                count = 0;
            }
            size_t end = start;
            while (end < text.size() && text[end] != '\r' && text[end] != '\n') ++end;
            batch[count++] = {prefix.data(), prefix.size()};
            const auto line = text.substr(start, end - start);
            batch[count++] = {line.data(), line.size()};
            batch[count++] = {"\n", 1};
            if (end == text.size()) break;
            start = end + 1;
            if (text[end] == '\r' && start < text.size() && text[start] == '\n') ++start;
        }
        if (count == batch.size()) {
            result_ = co_await writer_.writev(
                std::span<const body_buffer>(batch.data(), count), token);
            if (!result_.success()) co_return result_;
            count = 0;
        }
        batch[count++] = {"\n", 1};
        result_ = co_await writer_.writev(
            std::span<const body_buffer>(batch.data(), count), std::move(token));
        co_return result_;
    }

    body_writer& writer_;
    send_result result_;
};

namespace detail {
struct event_writer_access {
    static event_writer create(body_writer& writer) noexcept { return event_writer(writer); }
};
}

/// Own a move-only producer; default HTTP/1.1 framing is chunked. No CORS policy
/// is selected implicitly. The producer may customize the returned response's
/// headers before dispatch. It must not escape the supplied event_writer.
template<typename Producer>
    requires std::is_invocable_r_v<coro::task<send_result>,
                                  std::decay_t<Producer>&, event_writer&, coro::cancel_token>
streaming_response make_streaming_response(Producer&& producer) {
    response_head head(status::ok);
    head.set_header("Content-Type", "text/event-stream");
    head.set_header("Cache-Control", "no-cache");
    return streaming_response(std::move(head),
        [owned = std::forward<Producer>(producer)](
            body_writer& writer, coro::cancel_token token) mutable -> coro::task<send_result> {
            auto events = detail::event_writer_access::create(writer);
            try {
                const auto produced = co_await std::invoke(owned, events, std::move(token));
                co_return events.result().success() ? produced : events.result();
            } catch (...) {
                // A later producer exception cannot replace the first sink failure.
                if (events.result().success()) throw;
                co_return events.result();
            }
        });
}

} // namespace elio::http::sse
