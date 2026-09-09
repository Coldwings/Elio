#pragma once

#include <elio/http/http_response_head.hpp>
#include <elio/http/http_response_plan.hpp>
#include <elio/http/http_send_result.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>

#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace elio::http {

class body_writer;

namespace detail {
struct response_sender_access;
}

/// Owns a producer, not its externally referenced body buffers. A producer may
/// be move-only and runs at most once. Keep referenced captures alive through
/// producer completion and I/O cleanup; neither it nor the writer may escape.
class streaming_response : public response_head {
public:
    template<typename Producer>
        requires std::is_invocable_r_v<coro::task<send_result>,
                                      std::decay_t<Producer>&, body_writer&,
                                      coro::cancel_token>
    streaming_response(response_head head, Producer&& producer,
                       std::optional<uint64_t> length = std::nullopt,
                       response_transfer transfer = response_transfer::automatic)
        : response_head(std::move(head)),
          producer_(std::make_unique<producer_model<std::decay_t<Producer>>>(
              std::forward<Producer>(producer))),
          length_(length), transfer_(transfer) {}

    template<typename Producer>
        requires std::is_invocable_r_v<coro::task<send_result>,
                                      std::decay_t<Producer>&, body_writer&,
                                      coro::cancel_token>
    streaming_response(status code, Producer&& producer,
                       std::optional<uint64_t> length = std::nullopt,
                       response_transfer transfer = response_transfer::automatic)
        : streaming_response(response_head(code), std::forward<Producer>(producer),
                             length, transfer) {}

    streaming_response(const streaming_response&) = delete;
    streaming_response& operator=(const streaming_response&) = delete;
    streaming_response(streaming_response&&) noexcept = default;
    streaming_response& operator=(streaming_response&&) noexcept = default;

    std::optional<uint64_t> body_length() const noexcept { return length_; }
    response_transfer transfer() const noexcept { return transfer_; }

private:
    friend struct detail::response_sender_access;

    struct producer_base {
        virtual ~producer_base() = default;
        virtual coro::task<send_result> invoke(body_writer&, coro::cancel_token) = 0;
    };

    template<typename Producer>
    struct producer_model final : producer_base {
        template<typename Value>
        explicit producer_model(Value&& value) : producer(std::forward<Value>(value)) {}

        coro::task<send_result> invoke(body_writer& writer,
                                       coro::cancel_token token) override {
            co_return co_await std::invoke(producer, writer, std::move(token));
        }

        Producer producer;
    };

    coro::task<send_result> produce(body_writer& writer, coro::cancel_token token) {
        if (invoked_ || !producer_) {
            co_return send_result{send_errc::invalid_state};
        }
        invoked_ = true;
        co_return co_await producer_->invoke(writer, std::move(token));
    }

    std::unique_ptr<producer_base> producer_;
    std::optional<uint64_t> length_;
    response_transfer transfer_;
    bool invoked_ = false;
};

namespace detail {

struct response_sender_access {
    static bool can_produce(const streaming_response& response) noexcept {
        return response.producer_ && !response.invoked_;
    }

    static coro::task<send_result> produce(streaming_response& response,
                                           body_writer& writer,
                                           coro::cancel_token token) {
        return response.produce(writer, std::move(token));
    }
};

} // namespace detail
} // namespace elio::http
