#pragma once

#include <elio/coro/cancel_token.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
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
inline std::atomic<void (*)()> route_watchdog_before_construct_for_test{nullptr};
inline std::atomic<coro::task<void> (*)()> route_watchdog_after_start_for_test{nullptr};
inline std::atomic<void (*)()> route_operation_entered_for_test{nullptr};
inline std::atomic<coro::task<void> (*)()> route_operation_completed_for_test{nullptr};
#endif

inline coro::task<void> route_watchdog_task(
        std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<coro::cancel_source> stop,
        std::shared_ptr<std::atomic<bool>> expired,
        coro::cancel_token timer_token) {
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
}

inline coro::task<void> make_route_watchdog(
        std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<coro::cancel_source> stop,
        std::shared_ptr<std::atomic<bool>> expired,
        coro::cancel_token timer_token) {
#ifdef ELIO_RUNTIME_TEST_HOOKS
    if (auto hook = route_watchdog_before_construct_for_test.load(std::memory_order_acquire))
        hook();
#endif
    return route_watchdog_task(deadline, std::move(stop), std::move(expired),
                               std::move(timer_token));
}

template<typename Result>
struct route_operation_result {
    Result value;
    bool timed_out;
};

// A layered stream cannot be timed out by bypassing it through the TCP fd.
// Cancel the actual lower operation and join both tasks before its owner,
// buffers or published prefix can leave the enclosing exchange/setup frame.
template<typename Result, typename Operation, typename FailureCleanup = std::nullptr_t>
coro::task<route_operation_result<Result>> await_route_operation(Operation operation,
        coro::cancel_token token,
        std::optional<std::chrono::steady_clock::time_point> deadline,
        FailureCleanup cleanup_completed_on_failure = nullptr) {
    auto* scheduler = runtime::scheduler::current();
    if (!deadline || !scheduler) {
        auto value = co_await std::invoke(operation, token);
        co_return route_operation_result<Result>{std::move(value), false};
    }
    auto stop = std::make_shared<coro::cancel_source>();
    auto expired = std::make_shared<std::atomic<bool>>(false);
    auto forward = token.on_cancel([stop] { stop->cancel(); });
    coro::cancel_source timer_stop;
    // Construct the owning timer frame before admission or sibling I/O. A lazy
    // scheduler factory can fail outside the timer body's cancellation catch.
    auto watchdog = scheduler->go_joinable(make_route_watchdog(
        *deadline, stop, expired, timer_stop.get_token()));
#ifdef ELIO_RUNTIME_TEST_HOOKS
    if (auto hook = route_watchdog_after_start_for_test.load(std::memory_order_acquire))
        co_await hook();
#endif
    // Rejected independent admission returns an exceptional ready handle, but
    // an admitted watchdog can publish its result before its owning frame is
    // destroyed. Do not start or return sibling I/O until either kind settles.
    if (watchdog.is_ready()) {
        std::exception_ptr startup_failure;
        try { watchdog.await_resume(); }
        catch (...) { startup_failure = std::current_exception(); }
        try { co_await watchdog.wait_destroyed_async(); }
        catch (...) {
            if (!startup_failure) startup_failure = std::current_exception();
        }
        if (startup_failure) std::rethrow_exception(startup_failure);
    }
    std::optional<Result> value;
    std::exception_ptr failure;
    bool completed_late = false;
    try {
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = route_operation_entered_for_test.load(std::memory_order_acquire)) hook();
#endif
        value.emplace(co_await std::invoke(operation, stop->get_token()));
        completed_late = std::chrono::steady_clock::now() >= *deadline;
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = route_operation_completed_for_test.load(std::memory_order_acquire))
            co_await hook();
#endif
    } catch (...) {
        failure = std::current_exception();
    }
    try { timer_stop.cancel(); }
    catch (...) { if (!failure) failure = std::current_exception(); }
    try { co_await watchdog; }
    catch (...) { if (!failure) failure = std::current_exception(); }
    try { co_await watchdog.wait_destroyed_async(); }
    catch (...) { if (!failure) failure = std::current_exception(); }
    if (failure) {
        // The operation may have transferred an owning result before its
        // watchdog reports an independent failure. Let the caller retire that
        // completed value inside this frame instead of destroying it blindly.
        if (value) {
            if constexpr (!std::same_as<std::remove_cvref_t<FailureCleanup>,
                                         std::nullptr_t>) {
                try { co_await std::invoke(cleanup_completed_on_failure, *value); }
                catch (...) {}
            }
        }
        std::rethrow_exception(failure);
    }
    co_return route_operation_result<Result>{std::move(*value),
        completed_late || expired->load(std::memory_order_acquire)};
}

} // namespace elio::http::detail
