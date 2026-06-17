#pragma once

#include <cassert>
#include <chrono>
#include <optional>
#include <type_traits>
#include <variant>

#include "task.hpp"
#include "traits.hpp"
#include "when_any.hpp"
#include "cancel_token.hpp"
#include "../time/timer.hpp"

namespace elio {

template<typename T>
struct timeout_result {
    bool timed_out;
    std::optional<T> value;

    explicit operator bool() const noexcept { return !timed_out; }
    T& operator*() & { assert(value.has_value() && "dereferencing timed-out result"); return *value; }
    const T& operator*() const& { assert(value.has_value() && "dereferencing timed-out result"); return *value; }
    T&& operator*() && { assert(value.has_value() && "dereferencing timed-out result"); return std::move(*value); }
};

template<>
struct timeout_result<void> {
    bool timed_out;

    explicit operator bool() const noexcept { return !timed_out; }
};

/// Run a callable with a timeout. The callable may optionally accept a
/// cancel_token for cooperative cancellation when the timeout fires.
///
/// Returns timeout_result<T> where T is the callable's return type.
/// Use operator bool() or .timed_out to check, operator*() to get the value.
///
/// Usage:
///   if (auto r = co_await with_timeout(200ms, [](cancel_token tok) -> task<int> {
///       co_return co_await remote_fetch(key, tok);
///   })) {
///       use(*r);
///   } else {
///       // timed out
///   }
template<typename Rep, typename Period, typename F>
    requires detail::when_any_callable<std::decay_t<F>>
auto with_timeout(std::chrono::duration<Rep, Period> timeout, F&& f)
    -> coro::task<timeout_result<detail::when_any_result_t<std::decay_t<F>>>>
{
    using T = detail::when_any_result_t<std::decay_t<F>>;

    auto [idx, result] = co_await when_any(
        std::forward<F>(f),
        [timeout](coro::cancel_token tok) -> coro::task<void> {
            co_await time::sleep_for(timeout, tok);
        }
    );

    if constexpr (std::is_void_v<T>) {
        co_return timeout_result<void>{idx != 0};
    } else {
        if (idx == 0) {
            co_return timeout_result<T>{false, std::get<0>(std::move(result))};
        } else {
            co_return timeout_result<T>{true, std::nullopt};
        }
    }
}

} // namespace elio
