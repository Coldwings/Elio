#pragma once

#include <cassert>
#include <climits>
#include <coroutine>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <utility>
#include "../coro/cancel_token.hpp"
#include "../detail/intrusive_list.hpp"
#include "../runtime/scheduler.hpp"
#include "detail/wake_state.hpp"

namespace elio::sync {

/// Coroutine-aware semaphore
class semaphore {
public:
    class acquire_waiter;
    class cancellable_acquire_waiter;
    class acquire_awaitable;
    class cancellable_acquire_awaitable;

    explicit semaphore(int initial_count = 0)
        : count_(initial_count) {
        assert(initial_count >= 0 && "semaphore initial count must be non-negative");
    }

    ~semaphore() {
        assert(waiters_.empty() && "semaphore destroyed with pending waiters");
    }

    // Non-copyable, non-movable
    semaphore(const semaphore&) = delete;
    semaphore& operator=(const semaphore&) = delete;
    semaphore(semaphore&&) = delete;
    semaphore& operator=(semaphore&&) = delete;

    class acquire_waiter : public elio::detail::intrusive_list_node<acquire_waiter> {
    public:
        explicit acquire_waiter(semaphore& s)
            : sem_(s)
            , wake_state_(detail::make_wake_state()) {}

        acquire_waiter(semaphore& s, bool cancellable)
            : sem_(s)
            , wake_state_(detail::make_wake_state())
            , cancellable_(cancellable) {}

        ~acquire_waiter() {
            // Fast path: if we never suspended, we were never enqueued,
            // so no wake function could hold a reference to us.
            if (!suspended_) return;

            detail::wake_state_ptr to_schedule;
            // Slow path: acquire mutex to prevent race with release()
            {
                std::lock_guard<std::mutex> guard(sem_.mutex_);
                if (this->is_linked()) {
                    sem_.waiters_.remove(this);
                    detail::cancel_wake_state(wake_state_);
                } else if (grant_pending_ && !resumed_) {
                    detail::cancel_wake_state(wake_state_);
                    grant_pending_ = false;
                    to_schedule = sem_.recover_cancelled_handoff_locked();
                } else {
                    detail::cancel_wake_state(wake_state_);
                }
            }

            if (to_schedule) {
                detail::schedule_wake_state(to_schedule);
            }
        }

        bool await_ready_impl() const {
            if (!cancellable_) {
                return sem_.try_acquire();
            }

            if (wake_state_->was_cancelled()) {
                return true;
            }
            if (!sem_.try_acquire()) {
                return false;
            }
            if (detail::claim_wake_state(wake_state_) !=
                detail::wake_action::rejected) {
                return true;
            }

            // Cancellation won after the permit was acquired.
            sem_.restore_cancelled_permit();
            return true;
        }

        bool await_suspend_impl(std::coroutine_handle<> awaiter) noexcept {
            if (!cancellable_) {
                std::lock_guard<std::mutex> guard(sem_.mutex_);
                if (sem_.count_ > 0) {
                    --sem_.count_;
                    return false;
                }

                wake_state_->set_handle(awaiter);
                sem_.waiters_.push_back(this);
                suspended_ = true;
                return true;
            }

            detail::wake_state_ptr to_schedule;
            {
                std::lock_guard<std::mutex> guard(sem_.mutex_);

                if (wake_state_->was_cancelled()) {
                    return false;
                }

                if (sem_.count_ > 0) {
                    --sem_.count_;
                    if (detail::claim_wake_state(wake_state_) !=
                        detail::wake_action::rejected) {
                        return false;
                    }

                    // Cancellation won the acquire race. Hand the permit to
                    // another live waiter, or restore it to the count.
                    to_schedule = sem_.recover_cancelled_handoff_locked();
                } else {
                    if (!wake_state_->set_handle_blocked(awaiter)) {
                        return false;
                    }
                    sem_.waiters_.push_back(this);
                    suspended_ = true;

                    if (wake_state_->unblock_after_publish()) {
                        return true;
                    }

                    sem_.waiters_.remove(this);
                    suspended_ = false;
                }
            }

            if (to_schedule) {
                detail::schedule_wake_state(to_schedule);
            }
            return false;
        }

        coro::cancel_result await_resume_impl() noexcept {
            if (!cancellable_) {
                resumed_ = true;
                grant_pending_ = false;
                suspended_ = false;
                return coro::cancel_result::completed;
            }

            if (wake_state_->was_cancelled()) {
                if (suspended_) {
                    std::lock_guard<std::mutex> guard(sem_.mutex_);
                    if (this->is_linked()) {
                        sem_.waiters_.remove(this);
                    }
                    suspended_ = false;
                }
                return coro::cancel_result::cancelled;
            }

            resumed_ = true;
            grant_pending_ = false;
            suspended_ = false;
            return coro::cancel_result::completed;
        }

    protected:
        const detail::wake_state_ptr& cancellation_wake_state() const noexcept {
            return wake_state_;
        }

    private:
        semaphore& sem_;
        detail::wake_state_ptr wake_state_;
        bool cancellable_ = false;
        bool suspended_ = false;  // True if enqueued in waiters_
        bool resumed_ = false;    // True after a popped waiter resumes normally
        bool grant_pending_ = false;  // True after release() transfers a permit

        friend class semaphore;
    };

    class cancellable_acquire_waiter : public acquire_waiter {
    public:
        cancellable_acquire_waiter(semaphore& s, coro::cancel_token token)
            : acquire_waiter(s, true) {
            cancel_registration_ = token.on_cancel(
                [state = cancellation_wake_state()] {
                state->request_cancel();
            });
        }

        ~cancellable_acquire_waiter() {
            cancel_registration_.unregister();
        }

        coro::cancel_result await_resume_cancellable() noexcept {
            cancel_registration_.unregister();
            return await_resume_impl();
        }

    private:
        coro::cancel_token::registration cancel_registration_;
    };

    class acquire_awaitable {
    public:
        explicit acquire_awaitable(semaphore& s) : waiter_(s) {}

        bool await_ready() const noexcept { return waiter_.await_ready_impl(); }
        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            return waiter_.await_suspend_impl(awaiter);
        }
        void await_resume() noexcept {
            (void)waiter_.await_resume_impl();
        }

    private:
        acquire_waiter waiter_;
    };

    class cancellable_acquire_awaitable {
    public:
        cancellable_acquire_awaitable(semaphore& s, coro::cancel_token token)
            : waiter_(s, std::move(token)) {}

        bool await_ready() const { return waiter_.await_ready_impl(); }
        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            return waiter_.await_suspend_impl(awaiter);
        }
        [[nodiscard("check whether a semaphore permit was acquired")]]
        coro::cancel_result await_resume() noexcept {
            return waiter_.await_resume_cancellable();
        }

    private:
        cancellable_acquire_waiter waiter_;
    };

    /// Acquire (decrement) the semaphore.
    auto acquire() {
        return acquire_awaitable(*this);
    }

    /// Acquire a permit, or return cancelled if the token wins the wait race.
    auto acquire(coro::cancel_token token) {
        return cancellable_acquire_awaitable(*this, std::move(token));
    }

    /// Try to acquire without waiting
    bool try_acquire() noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        if (count_ > 0) {
            --count_;
            return true;
        }
        return false;
    }

    /// Release (increment) the semaphore
    void release(int count = 1) {
        assert(count > 0 && "semaphore release count must be positive");

        std::vector<detail::wake_state_ptr> to_schedule;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            to_schedule.reserve(std::min(static_cast<size_t>(count),
                                         waiters_.size()));

            int remaining = count;
            while (remaining > 0) {
                auto state = claim_waiter_locked();
                if (!state) {
                    break;
                }
                to_schedule.push_back(std::move(state));
                --remaining;
            }

            assert(count_ <= INT_MAX - remaining &&
                   "semaphore count overflow");
            count_ += remaining;
        }
        // Schedule outside lock to avoid deadlock if schedule_handle()
        // resumes inline (trampoline path) and destructor re-acquires mutex.
        detail::schedule_wake_states(to_schedule);
    }

    /// Get current count
    int count() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return count_;
    }

private:
    mutable std::mutex mutex_;
    int count_;
    elio::detail::intrusive_list<acquire_waiter> waiters_;

    detail::wake_state_ptr claim_waiter_locked() noexcept {
        while (!waiters_.empty()) {
            auto* waiter = waiters_.pop_front();
            if (waiter->cancellable_) {
                if (detail::claim_wake_state(waiter->wake_state_) ==
                    detail::wake_action::rejected) {
                    continue;
                }
            }
            waiter->grant_pending_ = true;
            return waiter->wake_state_;
        }

        return nullptr;
    }

    detail::wake_state_ptr recover_cancelled_handoff_locked() noexcept {
        if (auto state = claim_waiter_locked()) {
            return state;
        }
        assert(count_ < INT_MAX && "semaphore count overflow");
        ++count_;
        return nullptr;
    }

    void restore_cancelled_permit() {
        detail::wake_state_ptr to_schedule;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            to_schedule = recover_cancelled_handoff_locked();
        }
        if (to_schedule) {
            detail::schedule_wake_state(to_schedule);
        }
    }
};

} // namespace elio::sync
