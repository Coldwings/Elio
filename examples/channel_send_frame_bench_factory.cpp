#include "channel_send_frame_bench_factory.hpp"

#include <elio/coro/task.hpp>

namespace elio::bench {

#if defined(__GNUC__) || defined(__clang__)
#define ELIO_BENCH_NOINLINE __attribute__((noinline))
#else
#define ELIO_BENCH_NOINLINE
#endif

template<size_t InlineBytes>
ELIO_BENCH_NOINLINE unstarted_frame make_send_frame(
        sync::channel<inline_payload<InlineBytes>>& channel) {
    auto operation = channel.send(inline_payload<InlineBytes>{});
    auto handle = coro::detail::task_access::release(std::move(operation));
    return unstarted_frame(
        std::coroutine_handle<>::from_address(handle.address()));
}

template<size_t InlineBytes>
ELIO_BENCH_NOINLINE unstarted_frame make_cancellable_send_frame(
        sync::channel<inline_payload<InlineBytes>>& channel,
        coro::cancel_token token) {
    auto operation = channel.send(
        inline_payload<InlineBytes>{}, std::move(token));
    auto handle = coro::detail::task_access::release(std::move(operation));
    return unstarted_frame(
        std::coroutine_handle<>::from_address(handle.address()));
}

template unstarted_frame make_send_frame<8>(
    sync::channel<inline_payload<8>>&);
template unstarted_frame make_send_frame<64>(
    sync::channel<inline_payload<64>>&);
template unstarted_frame make_send_frame<256>(
    sync::channel<inline_payload<256>>&);
template unstarted_frame make_send_frame<1024>(
    sync::channel<inline_payload<1024>>&);

template unstarted_frame make_cancellable_send_frame<8>(
    sync::channel<inline_payload<8>>&, coro::cancel_token);
template unstarted_frame make_cancellable_send_frame<64>(
    sync::channel<inline_payload<64>>&, coro::cancel_token);
template unstarted_frame make_cancellable_send_frame<256>(
    sync::channel<inline_payload<256>>&, coro::cancel_token);
template unstarted_frame make_cancellable_send_frame<1024>(
    sync::channel<inline_payload<1024>>&, coro::cancel_token);

#undef ELIO_BENCH_NOINLINE

}  // namespace elio::bench
