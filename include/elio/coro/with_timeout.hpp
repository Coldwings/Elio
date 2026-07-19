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

namespace detail {

enum class timeout_branch_result {
    expired,
    cancelled,
};

template<typename T, typename Result>
T take_timeout_value(Result&& result) {
    if constexpr (std::is_same_v<std::remove_cvref_t<Result>, T>) {
        return std::forward<Result>(result);
    } else {
        return std::get<0>(std::forward<Result>(result));
    }
}

template<typename Result>
timeout_branch_result take_timeout_branch(Result&& result) {
    if constexpr (std::is_same_v<
                      std::remove_cvref_t<Result>, timeout_branch_result>) {
        return std::forward<Result>(result);
    } else {
        return std::get<1>(std::forward<Result>(result));
    }
}

} // namespace detail

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

/// Run a callable with a structured timeout. The callable may optionally accept
/// its child cancel_token. When the deadline expires, cancellation is requested
/// for the callable and this operation waits for it to reach a terminal state
/// before returning a timed-out result.
///
/// Returns timeout_result<T> where T is the callable's return type.
/// Use operator bool() or .timed_out to check, operator*() to get the value.
///
/// Usage:
///   if (auto r = co_await with_timeout(200ms, [](coro::cancel_token tok) -> coro::task<int> {
///       co_return co_await remote_fetch(key, tok);
///   })) {
///       use(*r);
///   } else {
///       // timed out
///   }
template<typename Rep, typename Period, typename F>
    requires detail::when_any_callable<F>
auto with_timeout(std::chrono::duration<Rep, Period> timeout, F f)
    -> coro::task<timeout_result<detail::when_any_result_t<F>>>
{
    using T = detail::when_any_result_t<F>;

    auto [idx, result] = co_await when_any(
        std::move(f),
        [timeout](coro::cancel_token token)
            -> coro::task<detail::timeout_branch_result> {
            const auto outcome =
                co_await time::sleep_for(timeout, std::move(token));
            co_return outcome == coro::cancel_result::completed
                ? detail::timeout_branch_result::expired
                : detail::timeout_branch_result::cancelled;
        }
    );

    if constexpr (std::is_void_v<T>) {
        if (idx == 0) {
            co_return timeout_result<void>{false};
        }
    } else {
        if (idx == 0) {
            co_return timeout_result<T>{
                false,
                detail::take_timeout_value<T>(std::move(result))};
        }
    }

    if (detail::take_timeout_branch(std::move(result)) ==
        detail::timeout_branch_result::cancelled) {
        throw combinator_cancelled();
    }

    if constexpr (std::is_void_v<T>) {
        co_return timeout_result<void>{true};
    } else {
        co_return timeout_result<T>{true, std::nullopt};
    }
}

} // namespace elio
