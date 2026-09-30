#pragma once

#include <elio/http/client_result.hpp>
#include <elio/http/http_response_reader.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace elio::http {

struct streaming_response_options {
    /// Decoded payload bytes, not a buffer allocation or a wire-byte limit.
    size_t max_body_size = std::numeric_limits<size_t>::max();
    /// Cumulative wire bytes in informational responses before final headers.
    size_t max_informational_bytes = 16 * 1024 * 1024;
};

struct body_read_progress {
    size_t transferred = 0;
    bool complete = false;
};

using body_read_result = client_result<body_read_progress>;

class client;

/// Borrowed only within client::with_response's handler. Serialize reads and
/// await every started read before returning from the handler. Neither the
/// reader nor a read task may escape that scope. Destination storage remains
/// valid until normal awaited return, including cancellation/error return.
class response_body_reader final {
public:
    response_body_reader(const response_body_reader&) = delete;
    response_body_reader& operator=(const response_body_reader&) = delete;
    response_body_reader(response_body_reader&&) = delete;
    response_body_reader& operator=(response_body_reader&&) = delete;

    /// Copies one available payload fragment (possibly short) into destination.
    /// An empty destination does not consume input or establish completion.
    /// Observe complete=true, not transferred=0 alone, as successful EOF.
    /// Terminal errors are sticky; concurrent entry is rejected with EALREADY
    /// without changing the admitted read. No buffer access survives return.
    coro::task<body_read_result> read_into(std::span<char> destination,
                                         coro::cancel_token token = {}) {
        return read_into_impl(destination.data(), destination.size(), std::move(token));
    }

    coro::task<body_read_result> read_into(std::span<std::byte> destination,
                                         coro::cancel_token token = {}) {
        return read_into_impl(destination.data(), destination.size(), std::move(token));
    }

    /// Inspect only between reads, while the handler still owns this scope.
    bool complete() const noexcept { return complete_; }
    const std::optional<client_error>& error() const noexcept { return error_; }

private:
    friend class client;
    using next_operation =
        std::function<coro::task<client_result<response_read_result>>(coro::cancel_token)>;

    response_body_reader(next_operation next, coro::cancel_token scope_token,
                         size_t body_limit)
        : next_(std::move(next)), scope_token_(std::move(scope_token)),
          body_limit_(body_limit) {}

    coro::task<body_read_result> read_into_impl(void* destination, size_t capacity,
                                               coro::cancel_token token) {
        if (reading_.test_and_set(std::memory_order_acquire)) {
            co_return detail::make_client_error(EALREADY, client_stage::body);
        }
        struct read_lease {
            std::atomic_flag& flag;
            ~read_lease() { flag.clear(std::memory_order_release); }
        } lease{reading_};

        if (error_) co_return *error_;
        if (complete_) co_return body_read_progress{0, true};
        if (scope_token_.is_cancelled() || token.is_cancelled()) {
            error_ = detail::make_client_error(ECANCELED, client_stage::body);
            co_return *error_;
        }
        if (capacity == 0) {
            co_return body_read_progress{0, false};
        }

        auto cancelled = std::make_shared<coro::cancel_source>();
        auto scope_forward = scope_token_.on_cancel([cancelled] { cancelled->cancel(); });
        auto read_forward = token.on_cancel([cancelled] { cancelled->cancel(); });
        while (pending_.empty()) {
            client_result<response_read_result> next;
            try {
                next = co_await next_(cancelled->get_token());
            } catch (...) {
                // Even if the application catches a transport exception, this
                // exchange must not later become eligible for pooling.
                error_ = detail::make_client_error(EIO, client_stage::body);
                throw;
            }
            if (const auto* failure = std::get_if<client_error>(&next)) {
                error_ = *failure;
                co_return *error_;
            }
            const auto part = std::get<response_read_result>(next);
            if (part.event == response_event::message_complete) {
                complete_ = true;
                co_return body_read_progress{0, true};
            }
            if (part.event != response_event::body) continue;
            if (part.body.size() > body_limit_ - body_bytes_) {
                error_ = detail::make_client_error(EMSGSIZE, client_stage::body);
                co_return *error_;
            }
            body_bytes_ += part.body.size();
            pending_ = part.body;
        }
        // Do not advance the decoder while any part of its borrowed scratch
        // view remains. Do not perform another receive after positive progress.
        const auto count = std::min(capacity, pending_.size());
        std::memcpy(destination, pending_.data(), count);
        pending_.remove_prefix(count);
        co_return body_read_progress{count, false};
    }

    next_operation next_;
    coro::cancel_token scope_token_;
    size_t body_limit_;
    size_t body_bytes_ = 0;
    std::string_view pending_;
    std::optional<client_error> error_;
    bool complete_ = false;
    std::atomic_flag reading_ = ATOMIC_FLAG_INIT;
};

} // namespace elio::http
