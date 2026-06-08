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

#include "task.hpp"
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
    std::atomic<void*> waiter_{nullptr};
    std::tuple<std::optional<when_all_slot_t<callable_result_t<Fs>>>...> values_;
    std::exception_ptr first_exception_;
    std::atomic<bool> has_exception_{false};

    explicit when_all_state(size_t count) : remaining_(count) {}

    void complete_one() {
        if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            void* addr = waiter_.exchange(nullptr, std::memory_order_acq_rel);
            if (addr) {
                runtime::schedule_handle(
                    std::coroutine_handle<>::from_address(addr));
            }
        }
    }

    void store_exception(std::exception_ptr ex) {
        bool expected = false;
        if (has_exception_.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) {
            first_exception_ = std::move(ex);
        }
    }
};

template<typename... Fs>
struct when_all_awaitable {
    using state_type = when_all_state<Fs...>;
    std::shared_ptr<state_type> state_;
    std::tuple<Fs...> callables_;

    explicit when_all_awaitable(Fs... fs)
        : state_(std::make_shared<state_type>(sizeof...(Fs)))
        , callables_(std::move(fs)...) {}

    bool await_ready() const noexcept { return false; }

    template<size_t... Is>
    void spawn_all(std::index_sequence<Is...>) {
        (elio::go([state = this->state_,
                   f = std::move(std::get<Is>(callables_))]() mutable
                      -> coro::task<void> {
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
        void* expected = nullptr;
        state_->waiter_.compare_exchange_strong(expected, awaiter.address(),
            std::memory_order_release, std::memory_order_relaxed);

        spawn_all(std::index_sequence_for<Fs...>{});

        if (state_->remaining_.load(std::memory_order_acquire) == 0) {
            void* addr = state_->waiter_.exchange(nullptr, std::memory_order_acq_rel);
            if (addr) return false;
        }
        return true;
    }

    auto await_resume() {
        if (state_->first_exception_) {
            std::rethrow_exception(state_->first_exception_);
        }
        return extract_values(std::index_sequence_for<Fs...>{});
    }

private:
    template<size_t... Is>
    auto extract_values(std::index_sequence<Is...>) {
        if constexpr (sizeof...(Fs) == 1) {
            using T = callable_result_t<std::tuple_element_t<0, std::tuple<Fs...>>>;
            if constexpr (std::is_void_v<T>) {
                return;
            } else {
                return std::move(*std::get<0>(state_->values_));
            }
        } else {
            return std::tuple{std::move(*std::get<Is>(state_->values_))...};
        }
    }
};

} // namespace detail

/// Await multiple callables concurrently, resuming when all complete.
/// Each callable must return a task<T>. Returns a tuple of results.
/// If any task throws, the first exception is propagated.
///
/// Usage:
///   auto [a, b] = co_await elio::when_all(
///       []() -> task<int> { co_return 1; },
///       []() -> task<int> { co_return 2; }
///   );
template<typename... Fs>
    requires ((std::invocable<Fs> && detail::is_task_v<std::invoke_result_t<Fs>>) && ...)
auto when_all(Fs... fs) {
    return detail::when_all_awaitable<Fs...>(std::move(fs)...);
}

} // namespace elio
