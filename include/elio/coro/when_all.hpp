#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "traits.hpp"
#include "cancel_token.hpp"
#include "detail/completion_waiter.hpp"
#include "../log/macros.hpp"
#include "../runtime/spawn.hpp"

namespace elio {

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
struct when_all_state {
    std::atomic<size_t> remaining_;
    coro::detail::completion_waiter_slot waiter_;
    std::tuple<std::optional<when_all_slot_t<callable_result_t<Fs>>>...> values_;
    std::exception_ptr first_exception_;
    std::atomic<bool> has_exception_{false};
    coro::cancel_source cancel_source_;

    explicit when_all_state(size_t count) : remaining_(count) {}

    void complete_one() {
        if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            auto waiter = waiter_.take();
            if (waiter) {
                runtime::schedule_handle(waiter);
            }
        }
    }

    bool set_waiter(coro::detail::completion_waiter& waiter,
                    std::coroutine_handle<> handle) noexcept {
        return waiter_.register_waiter(waiter, handle, [this] {
            return remaining_.load(std::memory_order_acquire) == 0;
        });
    }

    void store_exception(std::exception_ptr ex) {
        bool expected = false;
        if (has_exception_.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) {
            first_exception_ = std::move(ex);
            cancel_source_.cancel();
        } else {
            ELIO_LOG_WARNING("when_all: discarding subsequent exception "
                             "(only the first is propagated)");
        }
    }
};

template<typename... Fs>
struct when_all_awaitable {
    using state_type = when_all_state<Fs...>;
    using callables_type = std::tuple<Fs...>;
    std::shared_ptr<state_type> state_;
    coro::detail::completion_waiter waiter_;
    callables_type callables_;

    explicit when_all_awaitable(Fs... fs)
        : state_(std::make_shared<state_type>(sizeof...(Fs)))
        , waiter_(state_->waiter_)
        , callables_(std::move(fs)...) {}

    when_all_awaitable(when_all_awaitable&&)
        noexcept(std::is_nothrow_move_constructible_v<callables_type>) = default;

    when_all_awaitable& operator=(when_all_awaitable&& other)
        noexcept(std::is_nothrow_move_assignable_v<callables_type>)
        requires std::is_move_assignable_v<callables_type> {
        if (this != &other) {
            waiter_ = std::move(other.waiter_);
            state_ = std::move(other.state_);
            callables_ = std::move(other.callables_);
        }
        return *this;
    }

    bool await_ready() const noexcept { return false; }

    template<size_t... Is>
    void spawn_all(std::index_sequence<Is...>) {
        auto token = state_->cancel_source_.get_token();
        (elio::go([state = this->state_, token,
                   f = std::move(std::get<Is>(callables_))]() mutable
                      -> coro::task<void> {
            if (token.is_cancelled()) {
                state->complete_one();
                co_return;
            }
            using T = callable_result_t<std::tuple_element_t<Is, std::tuple<Fs...>>>;
            try {
                if constexpr (std::is_void_v<T>) {
                    co_await f();
                    std::get<Is>(state->values_).emplace(std::monostate{});
                } else {
                    std::get<Is>(state->values_).emplace(co_await f());
                }
            } catch (...) {
                state->store_exception(std::current_exception());
            }
            state->complete_one();
        }), ...);
    }

    bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
        bool should_suspend = state_->set_waiter(waiter_, awaiter);

        spawn_all(std::index_sequence_for<Fs...>{});

        // Non-empty inputs suspend and rely on complete_one() ->
        // schedule_handle() for resumption. An empty input is already complete.
        // schedule_handle() provides sufficient internal
        // synchronization (mutex/atomic in the scheduler's mpsc_queue) to
        // establish happens-before between sub-task data writes and the
        // waiter's await_resume reads.  An inline fast-path (returning
        // false when remaining_ is already 0) would lack this synchronization:
        // the waiter's acquire load on remaining_ only synchronizes with
        // the decrement, not the subsequent data stores in complete_one().
        return should_suspend;
    }

    auto await_resume() {
        // Check for exceptions BEFORE extracting values. If any sub-task threw,
        // its corresponding values_ slot is disengaged (std::nullopt), and
        // extracting would dereference a disengaged optional → UB.
        if (state_->first_exception_) {
            std::rethrow_exception(state_->first_exception_);
        }
        // Only extract values if all tasks completed successfully
        return extract_values(std::index_sequence_for<Fs...>{});
    }

private:
    template<size_t... Is>
    auto extract_values(std::index_sequence<Is...>) {
        return std::tuple{std::move(*std::get<Is>(state_->values_))...};
    }
};

} // namespace detail

/// Await multiple callables concurrently, resuming when all complete.
/// Each callable must return a task<T>. Returns a tuple of results.
/// If any task throws, the first exception is propagated.
///
/// Usage:
///   auto [a, b] = co_await elio::when_all(
///       []() -> coro::task<int> { co_return 1; },
///       []() -> coro::task<int> { co_return 2; }
///   );
template<typename... Fs>
    requires ((std::invocable<Fs> && detail::is_task_v<std::invoke_result_t<Fs>>) && ...)
auto when_all(Fs... fs) {
    return detail::when_all_awaitable<Fs...>(std::move(fs)...);
}

} // namespace elio
