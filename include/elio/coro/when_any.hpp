#pragma once

// EXPERIMENTAL: when_any's cancellation semantics are not yet fully validated.
// Define ELIO_EXPERIMENTAL to include this header via elio.hpp, or include it directly.

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
#include "when_all.hpp"
#include "cancel_token.hpp"
#include "../runtime/spawn.hpp"

namespace elio {

namespace detail {

template<typename... Fs>
struct when_any_state {
    using result_type = std::variant<when_all_slot_t<callable_result_t<Fs>>...>;

    std::atomic<bool> resolved_{false};
    std::atomic<void*> waiter_{nullptr};
    result_type result_;
    std::exception_ptr exception_;
    size_t winner_index_{0};
    coro::cancel_source cancel_source_;

    template<size_t I, typename T>
    void resolve(T&& value) {
        bool expected = false;
        if (resolved_.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) {
            winner_index_ = I;
            result_.template emplace<I>(std::forward<T>(value));
            cancel_source_.cancel();
            resume_waiter();
        }
    }

    template<size_t I>
    void resolve_void() {
        bool expected = false;
        if (resolved_.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) {
            winner_index_ = I;
            result_.template emplace<I>(std::monostate{});
            cancel_source_.cancel();
            resume_waiter();
        }
    }

    void resolve_exception(std::exception_ptr ex) {
        bool expected = false;
        if (resolved_.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) {
            exception_ = std::move(ex);
            cancel_source_.cancel();
            resume_waiter();
        }
    }

    void resume_waiter() {
        void* addr = waiter_.exchange(nullptr, std::memory_order_acq_rel);
        if (addr) {
            runtime::schedule_handle(
                std::coroutine_handle<>::from_address(addr));
        }
    }
};

template<typename... Fs>
struct when_any_awaitable {
    using state_type = when_any_state<Fs...>;
    std::shared_ptr<state_type> state_;
    std::tuple<Fs...> callables_;

    explicit when_any_awaitable(Fs... fs)
        : state_(std::make_shared<state_type>())
        , callables_(std::move(fs)...) {}

    bool await_ready() const noexcept { return false; }

    template<size_t... Is>
    void spawn_all(std::index_sequence<Is...>) {
        auto token = state_->cancel_source_.get_token();
        (elio::go([state = this->state_, token,
                   f = std::move(std::get<Is>(callables_))]() mutable
                      -> coro::task<void> {
            if (token.is_cancelled()) co_return;
            using T = callable_result_t<std::tuple_element_t<Is, std::tuple<Fs...>>>;
            try {
                if constexpr (std::is_void_v<T>) {
                    co_await f();
                    state->template resolve_void<Is>();
                } else {
                    auto val = co_await f();
                    state->template resolve<Is>(std::move(val));
                }
            } catch (...) {
                state->resolve_exception(std::current_exception());
            }
        }), ...);
    }

    bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
        void* expected = nullptr;
        state_->waiter_.compare_exchange_strong(expected, awaiter.address(),
            std::memory_order_release, std::memory_order_relaxed);

        spawn_all(std::index_sequence_for<Fs...>{});

        if (state_->resolved_.load(std::memory_order_acquire)) {
            void* addr = state_->waiter_.exchange(nullptr, std::memory_order_acq_rel);
            if (addr) return false;
        }
        return true;
    }

    auto await_resume() {
        if (state_->exception_) {
            std::rethrow_exception(state_->exception_);
        }
        return std::pair{state_->winner_index_, std::move(state_->result_)};
    }
};

} // namespace detail

/// Await multiple callables concurrently, resuming when the first one completes.
/// Each callable must return a task<T>. Returns {index, variant<results...>}.
/// Remaining tasks are signalled via cancel_token (cooperative cancellation).
///
/// Usage:
///   auto [idx, result] = co_await elio::when_any(
///       []() -> task<int> { co_return 1; },
///       []() -> task<int> { co_await sleep_for(10s); co_return 2; }
///   );
template<typename... Fs>
    requires ((std::invocable<Fs> && detail::is_task_v<std::invoke_result_t<Fs>>) && ...)
auto when_any(Fs... fs) {
    return detail::when_any_awaitable<Fs...>(std::move(fs)...);
}

} // namespace elio
