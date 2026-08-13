#pragma once

#include <array>
#include <cstddef>
#include <coroutine>
#include <utility>

#include <elio/coro/cancel_token.hpp>
#include <elio/sync/channel.hpp>

namespace elio::bench {

template<size_t InlineBytes>
struct inline_payload {
    std::array<std::byte, InlineBytes> bytes{};

    inline_payload() = default;
    inline_payload(const inline_payload&) = delete;
    inline_payload& operator=(const inline_payload&) = delete;
    inline_payload(inline_payload&&) noexcept = default;
    inline_payload& operator=(inline_payload&&) noexcept = default;
};

class unstarted_frame {
public:
    explicit unstarted_frame(std::coroutine_handle<> handle) noexcept
        : handle_(handle) {}

    unstarted_frame(const unstarted_frame&) = delete;
    unstarted_frame& operator=(const unstarted_frame&) = delete;

    unstarted_frame(unstarted_frame&& other) noexcept
        : handle_(std::exchange(other.handle_, {})) {}

    unstarted_frame& operator=(unstarted_frame&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~unstarted_frame() {
        if (handle_) handle_.destroy();
    }

    void* address() const noexcept { return handle_.address(); }

private:
    std::coroutine_handle<> handle_;
};

template<size_t InlineBytes>
unstarted_frame make_send_frame(
    sync::channel<inline_payload<InlineBytes>>& channel);

template<size_t InlineBytes>
unstarted_frame make_cancellable_send_frame(
    sync::channel<inline_payload<InlineBytes>>& channel,
    coro::cancel_token token);

extern template unstarted_frame make_send_frame<8>(
    sync::channel<inline_payload<8>>&);
extern template unstarted_frame make_send_frame<64>(
    sync::channel<inline_payload<64>>&);
extern template unstarted_frame make_send_frame<256>(
    sync::channel<inline_payload<256>>&);
extern template unstarted_frame make_send_frame<1024>(
    sync::channel<inline_payload<1024>>&);

extern template unstarted_frame make_cancellable_send_frame<8>(
    sync::channel<inline_payload<8>>&, coro::cancel_token);
extern template unstarted_frame make_cancellable_send_frame<64>(
    sync::channel<inline_payload<64>>&, coro::cancel_token);
extern template unstarted_frame make_cancellable_send_frame<256>(
    sync::channel<inline_payload<256>>&, coro::cancel_token);
extern template unstarted_frame make_cancellable_send_frame<1024>(
    sync::channel<inline_payload<1024>>&, coro::cancel_token);

}  // namespace elio::bench
