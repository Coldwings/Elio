#pragma once

#include <elio/coro/cancel_token.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace elio::http::detail {

#ifdef ELIO_RUNTIME_TEST_HOOKS
using route_operation_wait_hook = coro::task<coro::cancel_result> (*)(
    std::chrono::steady_clock::time_point, coro::cancel_token);
inline std::atomic<route_operation_wait_hook> route_operation_wait_for_test{nullptr};
#endif

template<typename Result>
struct route_operation_result {
    Result value;
    bool timed_out;
};

// A layered stream cannot be timed out by bypassing it through the TCP fd.
// Cancel the actual lower operation and join both tasks before its owner,
// buffers or published prefix can leave the enclosing exchange/setup frame.
template<typename Result, typename Operation>
coro::task<route_operation_result<Result>> await_route_operation(Operation operation,
        coro::cancel_token token,
        std::optional<std::chrono::steady_clock::time_point> deadline) {
    auto* scheduler = runtime::scheduler::current();
    if (!deadline || !scheduler)
        co_return route_operation_result<Result>{co_await std::invoke(operation, token), false};
    auto stop = std::make_shared<coro::cancel_source>();
    auto expired = std::make_shared<std::atomic<bool>>(false);
    auto forward = token.on_cancel([stop] { stop->cancel(); });
    coro::cancel_source timer_stop;
    auto watchdog = scheduler->go_joinable(
        [deadline = *deadline, stop, expired,
         timer_token = timer_stop.get_token()]() -> coro::task<void> {
            coro::cancel_result result;
            try {
#ifdef ELIO_RUNTIME_TEST_HOOKS
                if (auto hook = route_operation_wait_for_test.load(std::memory_order_acquire))
                    result = co_await hook(deadline, timer_token);
                else
#endif
                result = co_await time::sleep_for(deadline - std::chrono::steady_clock::now(),
                                                  timer_token);
            } catch (...) {
                auto failure = std::current_exception();
                try { stop->cancel(); } catch (...) {}
                std::rethrow_exception(failure);
            }
            if (result == coro::cancel_result::completed) {
                expired->store(true, std::memory_order_release);
                stop->cancel();
            }
        });
    std::optional<Result> value;
    std::exception_ptr failure;
    bool completed_late = false;
    try {
        value.emplace(co_await std::invoke(operation, stop->get_token()));
        completed_late = std::chrono::steady_clock::now() >= *deadline;
    } catch (...) {
        failure = std::current_exception();
    }
    try { timer_stop.cancel(); }
    catch (...) { if (!failure) failure = std::current_exception(); }
    try { co_await watchdog; }
    catch (...) { if (!failure) failure = std::current_exception(); }
    try { co_await watchdog.wait_destroyed_async(); }
    catch (...) { if (!failure) failure = std::current_exception(); }
    if (failure) std::rethrow_exception(failure);
    co_return route_operation_result<Result>{std::move(*value),
        completed_late || expired->load(std::memory_order_acquire)};
}

} // namespace elio::http::detail
