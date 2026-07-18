#pragma once

#include <atomic>
#include <coroutine>
#include <memory>
#include <vector>
#include "../../runtime/scheduler.hpp"

namespace elio::sync::detail {

enum class wake_action {
    rejected,
    completed_inline,
    schedule
};

class wake_state {
public:
    wake_state() = default;

    explicit wake_state(std::coroutine_handle<> handle) noexcept
        : handle_(handle)
        , state_(waiting) {}

    void set_handle(std::coroutine_handle<> handle) noexcept {
        handle_ = handle;
        auto expected = idle;
        state_.compare_exchange_strong(expected, waiting,
                                       std::memory_order_release,
                                       std::memory_order_acquire);
    }

    // Publish the handle before the awaiter is safe to resume. A notifier or
    // cancellation request that wins this window completes the await inline.
    bool set_handle_blocked(std::coroutine_handle<> handle) noexcept {
        handle_ = handle;
        auto expected = idle;
        return state_.compare_exchange_strong(expected, blocked,
                                              std::memory_order_release,
                                              std::memory_order_acquire);
    }

    // Finish the publication window. Once this returns true, another thread
    // may schedule the coroutine and the awaiter must no longer be accessed.
    bool unblock_after_publish() noexcept {
        auto expected = blocked;
        if (state_.compare_exchange_strong(expected, waiting,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            return true;
        }
        return false;
    }

    // Select normal completion. Resource-owning primitives call this while
    // holding their queue lock, before transferring ownership to the waiter.
    wake_action claim_notification() noexcept {
        auto current = state_.load(std::memory_order_acquire);
        for (;;) {
            unsigned char desired;
            wake_action action;
            switch (current) {
            case idle:
            case blocked:
                desired = notified_inline;
                action = wake_action::completed_inline;
                break;
            case waiting:
                desired = notification_claimed;
                action = wake_action::schedule;
                break;
            default:
                return wake_action::rejected;
            }

            if (state_.compare_exchange_weak(current, desired,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                return action;
            }
        }
    }

    // Cancellation is cooperative: it wins only if normal notification has
    // not already selected the terminal result.
    bool request_cancel() noexcept {
        auto current = state_.load(std::memory_order_acquire);
        for (;;) {
            unsigned char desired;
            bool should_schedule = false;
            switch (current) {
            case idle:
            case blocked:
                desired = cancelled_inline;
                break;
            case waiting:
                desired = cancellation_claimed;
                should_schedule = true;
                break;
            case cancelled_inline:
            case cancellation_claimed:
            case cancellation_scheduling:
                return true;
            default:
                return false;
            }

            if (state_.compare_exchange_weak(current, desired,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                if (should_schedule) {
                    schedule_claimed();
                }
                return true;
            }
        }
    }

    bool schedule_claimed() noexcept {
        auto expected = notification_claimed;
        if (state_.compare_exchange_strong(expected, notification_scheduling,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            runtime::schedule_handle(handle_);
            return true;
        }

        expected = cancellation_claimed;
        if (state_.compare_exchange_strong(expected, cancellation_scheduling,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            runtime::schedule_handle(handle_);
            return true;
        }
        return false;
    }

    // Schedule either an ordinary waiter or a waiter whose terminal result was
    // selected under its primitive's queue lock. The common legacy path needs
    // only the first waiting -> scheduling CAS.
    bool schedule_selected() noexcept {
        auto expected = waiting;
        if (state_.compare_exchange_strong(expected, notification_scheduling,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            runtime::schedule_handle(handle_);
            return true;
        }

        expected = notification_claimed;
        if (state_.compare_exchange_strong(expected, notification_scheduling,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            runtime::schedule_handle(handle_);
            return true;
        }

        expected = cancellation_claimed;
        if (state_.compare_exchange_strong(expected, cancellation_scheduling,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            runtime::schedule_handle(handle_);
            return true;
        }

        expected = blocked;
        state_.compare_exchange_strong(expected, notified_inline,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire);
        return false;
    }

    [[nodiscard]] bool was_cancelled() const noexcept {
        const auto current = state_.load(std::memory_order_acquire);
        return current == cancelled_inline ||
               current == cancellation_claimed ||
               current == cancellation_scheduling;
    }

    [[nodiscard]] bool notification_was_selected() const noexcept {
        const auto current = state_.load(std::memory_order_acquire);
        return current == notified_inline ||
               current == notification_claimed ||
               current == notification_scheduling;
    }

    // Prevent a dequeue-then-schedule wake from resuming a destroyed frame.
    void abandon() noexcept {
        auto current = state_.load(std::memory_order_acquire);
        for (;;) {
            switch (current) {
            case idle:
            case blocked:
            case waiting:
            case notified_inline:
            case cancelled_inline:
            case notification_claimed:
            case cancellation_claimed:
                break;
            default:
                return;
            }

            if (state_.compare_exchange_weak(current, abandoned,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                return;
            }
        }
    }

private:
    static constexpr unsigned char idle = 0;
    static constexpr unsigned char blocked = 1;
    static constexpr unsigned char waiting = 2;
    static constexpr unsigned char notified_inline = 3;
    static constexpr unsigned char cancelled_inline = 4;
    static constexpr unsigned char notification_claimed = 5;
    static constexpr unsigned char cancellation_claimed = 6;
    static constexpr unsigned char notification_scheduling = 7;
    static constexpr unsigned char cancellation_scheduling = 8;
    static constexpr unsigned char abandoned = 9;

    std::coroutine_handle<> handle_{};
    std::atomic<unsigned char> state_{idle};
};

using wake_state_ptr = std::shared_ptr<wake_state>;

inline wake_state_ptr make_wake_state() {
    return std::make_shared<wake_state>();
}

inline void cancel_wake_state(const wake_state_ptr& state) noexcept {
    if (state) {
        state->abandon();
    }
}

inline wake_action claim_wake_state(const wake_state_ptr& state) noexcept {
    return state ? state->claim_notification() : wake_action::rejected;
}

inline bool schedule_wake_state(const wake_state_ptr& state) noexcept {
    return state && state->schedule_selected();
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
