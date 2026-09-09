#pragma once

#include <elio/http/http_parser.hpp>
#include <elio/coro/task.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/io/io_backend.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <limits>
#include <string_view>
#include <vector>

namespace elio::http {

/// One pull from response_reader. error is a positive errno value on failure;
/// body borrows the reader's transport buffer until the next read, reset, move,
/// or destruction. A successful read is not necessarily a body event.
struct response_read_result {
    response_event event = response_event::need_more;
    std::string_view body;
    int error = 0;

    bool success() const noexcept { return error == 0; }
};

/// Pull-based HTTP/1 response decoding without an accumulating body buffer.
/// A supplied Stream must provide readiness-aware read(buffer, size, token).
/// Keep Stream alive and unmoved through each awaited read. Serialize all
/// reader operations; keep borrowed views only until the next operation.
/// Reading interims is explicit: observe completion, then next_response().
/// The reader never pools, closes, or retries a connection on the caller's
/// behalf. Transport errors preserve decoder state (e.g. an Expect wait may
/// cancel only its pending read); the transport contract determines whether a
/// subsequent read is valid. Framing errors are terminal until reset.
class response_reader {
public:
    explicit response_reader(size_t buffer_size = 8192)
        : buffer_(std::max(buffer_size, size_t{1})) {}

    response_reader(const response_reader&) = delete;
    response_reader& operator=(const response_reader&) = delete;
    response_reader(response_reader&&) = default;
    response_reader& operator=(response_reader&&) = default;

    const response_decoder& decoder() const noexcept { return decoder_; }
    void set_max_headers(size_t max) noexcept { decoder_.set_max_headers(max); }
    void set_max_header_size(size_t max) noexcept { decoder_.set_max_header_size(max); }
    void set_request_method(method value) noexcept { decoder_.set_request_method(value); }

    /// Discard the entire receive state before attaching to a new connection.
    /// Configured limits and buffer capacity are retained. No read may be active.
    void reset() {
        decoder_.reset();
        begin_ = end_ = message_bytes_ = 0;
        eof_ = handoff_ = completed_ = false;
    }

    /// Read one protocol event. Positive short reads and EINTR are handled
    /// internally. Cancellation is passed to the transport; return occurs only
    /// after that operation has completed its buffer access.
    template<typename Stream>
    coro::task<response_read_result> read(Stream& stream, coro::cancel_token token = {}) {
        auto receive = [&stream, token](void* data, size_t size)
            -> coro::task<io::io_result> {
            co_return co_await stream.read(data, size, token);
        };
        co_return co_await read_with(receive, token);
    }

    /// Adapter for a caller-owned deadline/read policy. receive must remain
    /// valid through the await and return io_result; it must not retain the
    /// supplied buffer after completion. This avoids a second decoding loop
    /// for deadline-controlled reads. Each retry uses the same receive policy.
    template<typename Receive>
    coro::task<response_read_result> read_with(
        Receive& receive, coro::cancel_token token = {}) {
        while (true) {
            auto input = std::string_view(buffer_.data() + begin_, end_ - begin_);
            auto result = decoder_.decode(input);
            begin_ += result.consumed;
            message_bytes_ += std::min(result.consumed,
                std::numeric_limits<size_t>::max() - message_bytes_);
            if (result.event != response_event::need_more) {
                handoff_ = result.event == response_event::protocol_handoff;
                completed_ = result.event == response_event::message_complete;
                co_return response_read_result{
                    result.event, result.body,
                    result.event == response_event::error
                        ? (decoder_.limit_exceeded() ? EMSGSIZE : EBADMSG) : 0};
            }
            if (token.is_cancelled()) {
                co_return response_read_result{response_event::error, {}, ECANCELED};
            }
            if (eof_) {
                auto done = decoder_.finish_eof();
                completed_ = done.event == response_event::message_complete;
                co_return response_read_result{
                    done.event, done.body,
                    done.event == response_event::error
                        ? (decoder_.limit_exceeded() ? EMSGSIZE : EBADMSG) : 0};
            }
            // need_more consumes all input, retaining only bounded framing
            // metadata. There is no body compaction/copy between reads.
            begin_ = end_ = 0;
            auto received = co_await receive(buffer_.data(), buffer_.size());
            if (received.result == -EINTR) {
                continue;
            }
            if (received.result < 0) {
                co_return response_read_result{
                    response_event::error, {}, static_cast<int>(-received.result)};
            }
            if (received.result == 0) {
                eof_ = true;
            } else {
                end_ = static_cast<size_t>(received.result);
            }
        }
    }

    /// Advance after a completed interim/final response, retaining bytes from
    /// the same transport read. Do not use this for a protocol handoff.
    /// Returns false without changing state if the current message is pending,
    /// malformed, or a handoff. Reapply request method after this returns true;
    /// both next_response() and reset() clear the method context.
    bool next_response() {
        if (!completed_ || decoder_.has_error() || handoff_) {
            return false;
        }
        decoder_.reset();
        completed_ = false;
        message_bytes_ = 0;
        return true;
    }

    size_t bytes_remaining() const noexcept { return end_ - begin_; }
    size_t message_bytes() const noexcept { return message_bytes_; }
    std::string_view remaining() const noexcept {
        return {buffer_.data() + begin_, end_ - begin_};
    }
    bool reached_eof() const noexcept { return eof_; }

private:
    response_decoder decoder_;
    std::vector<char> buffer_;
    size_t begin_ = 0;
    size_t end_ = 0;
    size_t message_bytes_ = 0;
    bool eof_ = false;
    bool handoff_ = false;
    bool completed_ = false;
};

} // namespace elio::http
