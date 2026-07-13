#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "traits.hpp"
#include "when_all.hpp"
#include "cancel_token.hpp"
#include "detail/completion_waiter.hpp"
#include "../log/macros.hpp"
#include "../runtime/scheduler.hpp"
#include "../runtime/spawn.hpp"

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
    (std::invocable<F, coro::cancel_token> && is_task_v<std::invoke_result_t<F, coro::cancel_token>>);

template<typename First, typename... Rest>
inline constexpr bool all_same_v = (std::is_same_v<First, Rest> && ...);

class when_any_wake_gate {
public:
    [[nodiscard]] bool publish_resolved() noexcept {
        return publish(kResolved);
    }

    [[nodiscard]] bool finish_launching() noexcept {
        return publish(kLaunchComplete);
    }

    [[nodiscard]] bool resolved() const noexcept {
        return (state_.load(std::memory_order_acquire) & kResolved) != 0;
    }

private:
    static constexpr std::uint8_t kResolved = 1U << 0;
    static constexpr std::uint8_t kLaunchComplete = 1U << 1;
    static constexpr std::uint8_t kResumeClaimed = 1U << 2;
    static constexpr std::uint8_t kReady = kResolved | kLaunchComplete;

    [[nodiscard]] bool publish(std::uint8_t flag) noexcept {
        const auto state = state_.fetch_or(flag, std::memory_order_acq_rel) | flag;
        if ((state & kReady) != kReady) {
            return false;
        }

        const auto previous = state_.fetch_or(
            kResumeClaimed, std::memory_order_acq_rel);
        return (previous & kResumeClaimed) == 0;
    }

    std::atomic<std::uint8_t> state_{0};
};

template<typename... Fs>
struct when_any_state {
    using result_type = std::variant<when_all_slot_t<when_any_result_t<Fs>>...>;

    std::atomic<bool> winner_claimed_{false};
    when_any_wake_gate wake_gate_;
    coro::detail::completion_waiter_slot waiter_;
    std::optional<result_type> result_;
    std::exception_ptr exception_;
    size_t winner_index_{0};
    coro::cancel_source cancel_source_;

    template<size_t I, typename... Args>
    void resolve(Args&&... args) noexcept {
        bool expected = false;
        if (winner_claimed_.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) {
            winner_index_ = I;
            try {
                result_.emplace(
                    std::in_place_index<I>, std::forward<Args>(args)...);
            } catch (...) {
                exception_ = std::current_exception();
            }
            cancel_losers();
            publish_resolved();
        }
    }

    void resolve_exception(std::exception_ptr ex) noexcept {
        bool expected = false;
        if (winner_claimed_.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) {
            exception_ = std::move(ex);
            cancel_losers();
            publish_resolved();
        } else {
            report_unhandled_exception(std::move(ex));
        }
    }

    void finish_launching() noexcept {
        if (wake_gate_.finish_launching()) {
            resume_waiter();
        }
    }

    bool set_waiter(coro::detail::completion_waiter& waiter,
                    std::coroutine_handle<> handle) noexcept {
        return waiter_.register_waiter(waiter, handle, [this] {
            return wake_gate_.resolved();
        });
    }

private:
    static void report_unhandled_exception(std::exception_ptr ex) noexcept {
        auto* sched = runtime::get_current_scheduler();
        if (sched) {
            sched->report_unhandled_exception(std::move(ex));
            return;
        }

        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            ELIO_LOG_ERROR("when_any unhandled exception (no scheduler): {}", e.what());
        } catch (...) {
            ELIO_LOG_ERROR("when_any unhandled exception (no scheduler): <unknown>");
        }
    }

    void cancel_losers() noexcept {
        try {
            cancel_source_.cancel();
        } catch (...) {
            report_unhandled_exception(std::current_exception());
        }
    }

    void publish_resolved() noexcept {
        if (wake_gate_.publish_resolved()) {
            resume_waiter();
        }
    }

    void resume_waiter() noexcept {
        auto waiter = waiter_.take();
        if (waiter) {
            runtime::schedule_handle(waiter);
        }
    }
};

template<typename... Fs>
struct when_any_awaitable {
    using state_type = when_any_state<Fs...>;
    using callables_type = std::tuple<Fs...>;
    std::shared_ptr<state_type> state_;
    coro::detail::completion_waiter waiter_;
    callables_type callables_;

    explicit when_any_awaitable(Fs... fs)
        : state_(std::make_shared<state_type>())
        , waiter_(state_->waiter_)
        , callables_(std::move(fs)...) {}

    when_any_awaitable(when_any_awaitable&&)
        noexcept(std::is_nothrow_move_constructible_v<callables_type>) = default;

    when_any_awaitable& operator=(when_any_awaitable&& other)
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
            if (token.is_cancelled()) co_return;
            using F = std::tuple_element_t<Is, std::tuple<Fs...>>;
            using T = when_any_result_t<F>;
            try {
                if constexpr (std::is_void_v<T>) {
                    if constexpr (std::invocable<F, coro::cancel_token>) {
                        co_await f(token);
                    } else {
                        co_await f();
                    }
                    state->template resolve<Is>(std::monostate{});
                } else if constexpr (std::invocable<F, coro::cancel_token>) {
                    auto val = co_await f(token);
                    state->template resolve<Is>(std::move(val));
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
        auto state = state_;
        bool should_suspend = state->set_waiter(waiter_, awaiter);

        spawn_all(std::index_sequence_for<Fs...>{});
        state->finish_launching();

        // Suspend and rely on resume_waiter() -> schedule_handle() for
        // resumption. The wake gate's acq_rel RMW links the winner's data
        // publication to whichever publisher claims the resume; scheduler
        // queue synchronization then completes the handoff to await_resume().
        // An inline fast-path (returning false when the wake gate is already
        // resolved) would not route the waiter through the scheduler queue,
        // so keep all non-empty resumptions on the scheduled path.
        return should_suspend;
    }

    auto await_resume() {
        if (state_->exception_) {
            std::rethrow_exception(state_->exception_);
        }
        if constexpr (all_same_v<when_all_slot_t<when_any_result_t<Fs>>...>) {
            using common_t = when_all_slot_t<when_any_result_t<
                std::tuple_element_t<0, std::tuple<Fs...>>>>;
            return std::pair{state_->winner_index_,
                std::visit([](auto&& v) -> common_t {
                    return std::forward<decltype(v)>(v);
                }, std::move(*state_->result_))};
        } else {
            return std::pair{state_->winner_index_, std::move(*state_->result_)};
        }
    }
};

} // namespace detail

/// Await multiple callables concurrently, resuming when the first one completes.
/// Each callable must return a task<T>. Callables may optionally accept a
/// cancel_token parameter for cooperative cancellation when another wins.
///
/// When all callables return the same type, result is that type directly;
/// otherwise it is std::variant<results...>.
///
/// Usage:
///   auto [idx, value] = co_await elio::when_any(
///       [](coro::cancel_token tok) -> coro::task<int> { co_return co_await fetch(key, tok); },
///       []() -> coro::task<int> { co_await sleep_for(10s); co_return -1; }
///   );
template<typename... Fs>
    requires (detail::when_any_callable<Fs> && ...)
auto when_any(Fs... fs) {
    return detail::when_any_awaitable<Fs...>(std::move(fs)...);
}

} // namespace elio
