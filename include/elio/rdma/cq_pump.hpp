#pragma once

/// @file cq_pump.hpp
/// @brief Default CQ-pump coroutine wiring a completion-channel fd
///        into an `io_context` (stage S7).
///
/// In real-world RDMA-Core usage, a CQ is associated with a
/// completion channel fd. When the verbs library wants to notify the
/// application that one or more CQEs are ready, it makes the fd
/// readable; the application then:
///
///   1. Reads the channel via `ibv_get_cq_event` to obtain the CQ
///      pointer that triggered the event.
///   2. Calls `ibv_poll_cq` in a loop to drain CQEs.
///   3. Calls `ibv_req_notify_cq` to re-arm.
///   4. Calls `ibv_ack_cq_events` periodically.
///
/// Steps 1–4 are backend-specific (libibverbs, librxe, vendor
/// providers). Elio's `cq_pump` provides only the io_context binding
/// piece: it polls the fd until readable, then hands control to a
/// caller-supplied `drain` callable. The callable is responsible for
/// the actual ibverbs sequence above and for invoking
/// `dispatcher::deliver(...)` for each CQE.
///
/// **Cancellation**: pass a `cancel_token` to stop the pump cleanly.
/// The loop checks the token between iterations, before and after
/// each poll. A blocked poll is registered with the token, so
/// cancelling the token aborts the in-flight wait without requiring
/// a synthetic fd wakeup.
///
/// **Error handling**: any non-recoverable poll error (errno other
/// than EINTR / EAGAIN / ECANCELED) ends the loop. A graceful close
/// of the fd manifests as the poll completing with `result == 0`
/// (POLLHUP-equivalent on epoll/io_uring); `drain` is called one
/// final time, then the loop exits.
///
/// Example:
///
/// @code{.cpp}
/// elio::rdma::dispatcher disp;
/// auto pump = [&]() -> elio::coro::task<void> {
///     co_await elio::rdma::cq_pump(
///         cq_channel_fd, disp,
///         [&](elio::rdma::dispatcher& d) {
///             // ibv_get_cq_event / ibv_poll_cq / d.deliver / ack /
///             // re-arm. Implementation owns the verbs sequence.
///         },
///         my_cancel_token);
/// };
/// sched.go(pump);
/// @endcode

#include <elio/coro/cancel_token.hpp>
#include <elio/coro/task.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/rdma/completion.hpp>

#include <cerrno>
#include <type_traits>

namespace elio::rdma {

namespace detail {

template <typename Drain>
concept cq_drain_callable = requires(Drain d, dispatcher& disp) {
    { d(disp) } noexcept;
};

}  // namespace detail

/// Run a CQ-pump loop on the given fd. Returns when the cancel
/// token fires or the fd reports a non-transient error.
///
/// @tparam Drain Callable invoked as `drain(dispatcher&)` each time
///         the fd reports readability. The callable should be
///         `noexcept` — exceptions thrown out of it during a CQE
///         drain are programmer errors.
/// @param fd     Completion-channel fd (typically the result of
///               `ibv_create_comp_channel` → `comp_channel->fd`).
/// @param disp   Dispatcher passed to the drain callable.
/// @param drain  User-supplied drain callable.
/// @param token  Optional cancel_token; if cancelled the loop exits
///               after the current poll returns.
template <detail::cq_drain_callable Drain>
[[nodiscard]] inline coro::task<void> cq_pump(int fd,
                                              dispatcher& disp,
                                              Drain drain,
                                              coro::cancel_token token = {}) {
    while (!token.is_cancelled()) {
        auto result = co_await io::async_poll_read(fd, token);

        if (token.is_cancelled()) {
            break;
        }

        if (!result.success()) {
            const int err = result.error_code();
            if (err == EINTR || err == EAGAIN || err == ECANCELED) {
                continue;  // benign — retry the poll
            }
            break;  // unrecoverable; let the user destroy the fd
        }

        drain(disp);

        // POLLHUP / graceful close: result == 0 means the fd was closed.
        // drain() was called one final time above; now exit the loop.
        if (result.io.result == 0) {
            break;
        }
    }
}

}  // namespace elio::rdma
