#pragma once

#include <atomic>
#include <coroutine>
#include <memory>
#include <vector>
#include "../../runtime/scheduler.hpp"

namespace elio::sync::detail {

class wake_state {
public:
    wake_state() = default;

    explicit wake_state(std::coroutine_handle<> handle) noexcept
        : handle_(handle) {}

    void set_handle(std::coroutine_handle<> handle) noexcept {
        handle_ = handle;
        state_.store(waiting, std::memory_order_release);
    }

    // Publish the handle before the awaiter is safe to resume. A notifier that
    // sees this state records the wakeup instead of scheduling immediately.
    void set_handle_blocked(std::coroutine_handle<> handle) noexcept {
        handle_ = handle;
        state_.store(blocked, std::memory_order_release);
    }

    // Finish the publication window. Return false when a wakeup arrived while
    // blocked so await_suspend continues the coroutine inline after unlock().
    bool unblock_after_publish() noexcept {
        auto expected = blocked;
        if (state_.compare_exchange_strong(expected, waiting,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            return true;
        }
        expected = notified_while_blocked;
        if (state_.compare_exchange_strong(expected, scheduling,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            return false;
        }
        return false;
    }

    void cancel() noexcept {
        auto expected = waiting;
        if (state_.compare_exchange_strong(expected, cancelled,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            return;
        }
        expected = blocked;
        if (state_.compare_exchange_strong(expected, cancelled,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            return;
        }
        expected = notified_while_blocked;
        state_.compare_exchange_strong(expected, cancelled,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire);
    }

    bool schedule_if_waiting() noexcept {
        auto expected = waiting;
        if (state_.compare_exchange_strong(expected, scheduling,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            runtime::schedule_handle(handle_);
            return true;
        }
        expected = blocked;
        if (state_.compare_exchange_strong(expected, notified_while_blocked,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            return false;
        }
        return false;
    }

private:
    static constexpr unsigned char waiting = 0;
    static constexpr unsigned char cancelled = 1;
    static constexpr unsigned char scheduling = 2;
    static constexpr unsigned char blocked = 3;
    static constexpr unsigned char notified_while_blocked = 4;

    std::atomic<unsigned char> state_{waiting};
    std::coroutine_handle<> handle_{};
};

using wake_state_ptr = std::shared_ptr<wake_state>;

inline wake_state_ptr make_wake_state() {
    return std::make_shared<wake_state>();
}

inline void cancel_wake_state(const wake_state_ptr& state) noexcept {
    if (state) {
        state->cancel();
    }
}

inline bool schedule_wake_state(const wake_state_ptr& state) noexcept {
    if (state) {
        return state->schedule_if_waiting();
    }
    return false;
}

template<typename Range>
size_t schedule_wake_states(const Range& states) noexcept {
    size_t scheduled = 0;
    for (const auto& state : states) {
        if (schedule_wake_state(state)) {
            ++scheduled;
        }
    }
    return scheduled;
}

} // namespace elio::sync::detail
