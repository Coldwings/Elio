#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "cancel_token.hpp"
#include "task_group.hpp"
#include "traits.hpp"
#include "when_all.hpp"

namespace elio {

namespace detail {

template<typename F, bool = std::invocable<F, coro::cancel_token>>
struct when_any_result;

template<typename F>
struct when_any_result<F, true> {
    using type = task_value_t<std::invoke_result_t<F, coro::cancel_token>>;
};

template<typename F>
struct when_any_result<F, false> {
    using type = task_value_t<std::invoke_result_t<F>>;
};

template<typename F>
using when_any_result_t = typename when_any_result<F>::type;

template<typename F>
concept when_any_callable =
    (std::invocable<F> && is_task_v<std::invoke_result_t<F>>) ||
    (std::invocable<F, coro::cancel_token> &&
     is_task_v<std::invoke_result_t<F, coro::cancel_token>>);

template<typename First, typename... Rest>
inline constexpr bool all_same_v = (std::is_same_v<First, Rest> && ...);

template<typename... Fs>
struct when_any_state {
    using result_type =
        std::variant<when_all_slot_t<when_any_result_t<Fs>>...>;

    std::atomic<bool> winner_claimed_{false};
    std::optional<result_type> result_;
    std::exception_ptr exception_;
    size_t winner_index_{0};

    template<size_t I, typename... Args>
    void resolve(coro::detail::task_group_state& group_state,
                 Args&&... args) noexcept {
        bool expected = false;
        if (!winner_claimed_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return;
        }

        winner_index_ = I;
        try {
            result_.emplace(
                std::in_place_index<I>, std::forward<Args>(args)...);
        } catch (...) {
            exception_ = std::current_exception();
        }
        request_combinator_cancel_noexcept(group_state);
    }

    void resolve_exception(coro::detail::task_group_state& group_state,
                           std::exception_ptr exception) noexcept {
        bool expected = false;
        if (winner_claimed_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            exception_ = std::move(exception);
            request_combinator_cancel_noexcept(group_state);
            return;
        }

        report_discarded_combinator_exception(std::move(exception));
    }

    auto take_result() {
        if constexpr (all_same_v<
                          when_all_slot_t<when_any_result_t<Fs>>...>) {
            using first_callable =
                std::tuple_element_t<0, std::tuple<Fs...>>;
            using common_type =
                when_all_slot_t<when_any_result_t<first_callable>>;
            return std::pair{
                winner_index_,
                std::visit(
                    [](auto&& value) -> common_type {
                        return std::forward<decltype(value)>(value);
                    },
                    std::move(*result_))};
        } else {
            return std::pair{winner_index_, std::move(*result_)};
        }
    }
};

template<typename... Fs>
using when_any_output_t =
    decltype(std::declval<when_any_state<Fs...>&>().take_result());

template<size_t I, typename Callables, typename State>
void spawn_when_any_child(coro::task_group& group,
                          Callables& callables,
                          const std::shared_ptr<State>& state,
                          const std::shared_ptr<
                              coro::detail::task_group_state>& group_state) {
    using F = std::remove_reference_t<
        std::tuple_element_t<I, Callables>>;
    using T = when_any_result_t<F>;

    group.spawn(
        [function = std::move(std::get<I>(callables)),
         state,
         group_state]() mutable -> coro::task<void> {
            try {
                auto token = coro::this_coro::cancel_token();
                if constexpr (std::is_void_v<T>) {
                    if constexpr (std::invocable<F, coro::cancel_token>) {
                        co_await std::invoke(
                            std::move(function), std::move(token));
                    } else {
                        co_await std::invoke(std::move(function));
                    }
                    state->template resolve<I>(
                        *group_state, std::monostate{});
                } else if constexpr (
                    std::invocable<F, coro::cancel_token>) {
                    auto value = co_await std::invoke(
                        std::move(function), std::move(token));
                    state->template resolve<I>(
                        *group_state, std::move(value));
                } else {
                    auto value =
                        co_await std::invoke(std::move(function));
                    state->template resolve<I>(
                        *group_state, std::move(value));
                }
            } catch (...) {
                state->resolve_exception(
                    *group_state, std::current_exception());
            }
        });
}

template<typename Callables, typename State, size_t... Is>
void spawn_when_any_children(coro::task_group& group,
                             Callables& callables,
                             const std::shared_ptr<State>& state,
                             const std::shared_ptr<
                                 coro::detail::task_group_state>& group_state,
                             std::index_sequence<Is...>) {
    (spawn_when_any_child<Is>(
         group, callables, state, group_state), ...);
}

} // namespace detail

/// Await task-producing callables concurrently and select the first branch to
/// complete, including exceptional completion. Callables may accept the
/// structured child cancel_token. Once a winner is selected, cancellation is
/// requested for every unfinished sibling and the combinator waits for all
/// accepted children to reach a terminal state before returning or throwing.
///
/// A losing exception is reported through the scheduler's unhandled-exception
/// handler after the winner has already been fixed. Parent cancellation that
/// drains every branch before any result is produced throws
/// combinator_cancelled.
template<typename... Fs>
    requires (sizeof...(Fs) > 0 &&
              (detail::when_any_callable<Fs> && ...))
auto when_any(Fs... fs)
    -> coro::task<detail::when_any_output_t<Fs...>> {
    auto state = std::make_shared<detail::when_any_state<Fs...>>();
    auto callables = std::forward_as_tuple(fs...);
    coro::task_group group;
    auto group_state =
        coro::detail::task_group_access::shared_state(group);

    std::exception_ptr launch_failure;
    try {
        detail::spawn_when_any_children(
            group, callables, state, group_state,
            std::index_sequence_for<Fs...>{});
    } catch (...) {
        launch_failure = std::current_exception();
        detail::request_combinator_cancel_noexcept(group);
    }

    std::exception_ptr join_failure;
    try {
        co_await group.join();
    } catch (...) {
        join_failure = std::current_exception();
    }

    if (launch_failure) {
        std::rethrow_exception(std::move(launch_failure));
    }
    if (join_failure) {
        std::rethrow_exception(std::move(join_failure));
    }
    if (!state->winner_claimed_.load(std::memory_order_acquire)) {
        throw combinator_cancelled();
    }
    if (state->exception_) {
        std::rethrow_exception(std::move(state->exception_));
    }

    co_return state->take_result();
}

} // namespace elio
