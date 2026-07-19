#pragma once

#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "task_group.hpp"
#include "traits.hpp"
#include "../log/macros.hpp"

namespace elio {

/// Raised when parent cancellation prevents a structured combinator from
/// producing the result required by its return type.
class combinator_cancelled final : public std::runtime_error {
public:
    combinator_cancelled()
        : std::runtime_error(
              "structured combinator cancelled before producing a result") {}
};

namespace detail {

template<typename F>
using callable_result_t = task_value_t<std::invoke_result_t<F>>;

template<typename T>
struct when_all_slot { using type = T; };
template<>
struct when_all_slot<void> { using type = std::monostate; };
template<typename T>
using when_all_slot_t = typename when_all_slot<T>::type;

template<typename... Fs>
using when_all_result_t =
    std::tuple<when_all_slot_t<callable_result_t<Fs>>...>;

template<typename... Fs>
using when_all_storage_t =
    std::tuple<std::optional<when_all_slot_t<callable_result_t<Fs>>>...>;

inline void report_discarded_combinator_exception(
    std::exception_ptr exception) noexcept {
    if (auto* scheduler = runtime::get_current_scheduler()) {
        scheduler->report_unhandled_exception(std::move(exception));
        return;
    }

    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        ELIO_LOG_ERROR("unhandled structured combinator exception: {}",
                       error.what());
    } catch (...) {
        ELIO_LOG_ERROR("unhandled structured combinator exception: <unknown>");
    }
}

inline void request_combinator_cancel_noexcept(
    coro::task_group& group) noexcept {
    try {
        group.request_cancel();
    } catch (...) {
        report_discarded_combinator_exception(std::current_exception());
    }
}

template<size_t I, typename Callables, typename Values>
void spawn_when_all_child(coro::task_group& group,
                          Callables& callables,
                          Values& values) {
    using F = std::remove_reference_t<
        std::tuple_element_t<I, Callables>>;
    using T = callable_result_t<F>;

    group.spawn(
        [function = std::move(std::get<I>(callables)),
         &slot = std::get<I>(values)]() mutable -> coro::task<void> {
            if constexpr (std::is_void_v<T>) {
                co_await std::invoke(std::move(function));
                slot.emplace(std::monostate{});
            } else {
                slot.emplace(co_await std::invoke(std::move(function)));
            }
        });
}

template<typename Callables, typename Values, size_t... Is>
void spawn_when_all_children(coro::task_group& group,
                             Callables& callables,
                             Values& values,
                             std::index_sequence<Is...>) {
    (spawn_when_all_child<Is>(group, callables, values), ...);
}

template<typename Values, size_t... Is>
[[nodiscard]] bool all_when_all_values_present(
    const Values& values, std::index_sequence<Is...>) noexcept {
    return (std::get<Is>(values).has_value() && ...);
}

template<typename... Fs, size_t... Is>
auto extract_when_all_values(when_all_storage_t<Fs...>& values,
                             std::index_sequence<Is...>)
    -> when_all_result_t<Fs...> {
    return when_all_result_t<Fs...>{
        std::move(*std::get<Is>(values))...};
}

} // namespace detail

/// Await multiple task-producing callables concurrently in one structured
/// scheduler-bound group. The first child failure requests cancellation of
/// unfinished siblings, but the combinator does not return until every accepted
/// child reaches a terminal state. The first failure is then rethrown.
///
/// Parent cancellation is propagated through the group. If it prevents one or
/// more result slots from being produced, the combinator throws
/// combinator_cancelled after all children have drained.
template<typename... Fs>
    requires ((std::invocable<Fs> &&
               detail::is_task_v<std::invoke_result_t<Fs>>) && ...)
auto when_all(Fs... fs)
    -> coro::task<detail::when_all_result_t<Fs...>> {
    if constexpr (sizeof...(Fs) == 0) {
        co_return detail::when_all_result_t<Fs...>{};
    } else {
        detail::when_all_storage_t<Fs...> values;
        auto callables = std::forward_as_tuple(fs...);
        coro::task_group group;

        std::exception_ptr launch_failure;
        try {
            detail::spawn_when_all_children(
                group, callables, values, std::index_sequence_for<Fs...>{});
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
        if (!detail::all_when_all_values_present(
                values, std::index_sequence_for<Fs...>{})) {
            throw combinator_cancelled();
        }

        co_return detail::extract_when_all_values<Fs...>(
            values, std::index_sequence_for<Fs...>{});
    }
}

} // namespace elio
