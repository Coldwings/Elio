#pragma once

#include <coroutine>
#include <atomic>
#include <mutex>
#include <vector>
#include <cassert>
#include <utility>
#include "../coro/cancel_token.hpp"
#include "../detail/intrusive_list.hpp"
#include "../runtime/scheduler.hpp"
#include "detail/wake_state.hpp"

namespace elio::sync {

/// Coroutine-aware event (manual reset)
class event {
public:
    class event_waiter;
    class cancellable_event_waiter;
    class wait_awaitable;
    class cancellable_wait_awaitable;

    event() = default;

    ~event() {
        assert(waiters_.empty() && "event destroyed with pending waiters");
    }

    // Non-copyable, non-movable
    event(const event&) = delete;
    event& operator=(const event&) = delete;
    event(event&&) = delete;
    event& operator=(event&&) = delete;

    class event_waiter : public elio::detail::intrusive_list_node<event_waiter> {
    public:
        explicit event_waiter(event& e)
            : evt_(e)
            , wake_state_(detail::make_wake_state()) {}

        event_waiter(event& e, bool cancellable)
            : evt_(e)
            , wake_state_(detail::make_wake_state())
            , cancellable_(cancellable) {}

        ~event_waiter() {
            // Fast path: if we never suspended, we were never enqueued,
            // so no wake function could hold a reference to us.
            if (!suspended_) return;

            // Slow path: acquire mutex to serialize access to the waiter list.
            // Either we unlink before set() pops us, or set() has already
            // popped us (so is_linked() is false here). This ensures we don't
            // race with set()'s collect-then-schedule pattern.
            std::lock_guard<std::mutex> guard(evt_.mutex_);
            if (this->is_linked()) {
                evt_.waiters_.remove(this);
            }
            detail::cancel_wake_state(wake_state_);
        }

        bool await_ready_impl() const noexcept {
            if (!cancellable_) {
                return evt_.signaled_.load(std::memory_order_acquire);
            }

            if (wake_state_->was_cancelled()) {
                return true;
            }
            if (!evt_.signaled_.load(std::memory_order_acquire)) {
                return false;
            }
            detail::claim_wake_state(wake_state_);
            return true;
        }

        bool await_suspend_impl(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(evt_.mutex_);

            if (!cancellable_) {
                if (evt_.signaled_.load(std::memory_order_relaxed)) {
                    return false;
                }

                wake_state_->set_handle(awaiter);
                evt_.waiters_.push_back(this);
                suspended_ = true;
                return true;
            }

            if (wake_state_->was_cancelled()) {
                return false;
            }

            if (evt_.signaled_.load(std::memory_order_relaxed)) {
                detail::claim_wake_state(wake_state_);
                return false;
            }

            if (!wake_state_->set_handle_blocked(awaiter)) {
                return false;
            }
            evt_.waiters_.push_back(this);
            suspended_ = true;

            if (wake_state_->unblock_after_publish()) {
                return true;
            }

            evt_.waiters_.remove(this);
            suspended_ = false;
            return false;
        }

        coro::cancel_result await_resume_impl() noexcept {
            if (!cancellable_) {
                suspended_ = false;
                return coro::cancel_result::completed;
            }

            if (wake_state_->was_cancelled()) {
                if (suspended_) {
                    std::lock_guard<std::mutex> guard(evt_.mutex_);
                    if (this->is_linked()) {
                        evt_.waiters_.remove(this);
                    }
                    suspended_ = false;
                }
                return coro::cancel_result::cancelled;
            }

            suspended_ = false;
            return coro::cancel_result::completed;
        }

    protected:
        const detail::wake_state_ptr& cancellation_wake_state() const noexcept {
            return wake_state_;
        }

    private:
        event& evt_;
        detail::wake_state_ptr wake_state_;
        bool cancellable_ = false;
        bool suspended_ = false;  // True if enqueued in waiters_

        friend class event;
    };

    class cancellable_event_waiter : public event_waiter {
    public:
        cancellable_event_waiter(event& e, coro::cancel_token token)
            : event_waiter(e, true) {
            cancel_registration_ = token.on_cancel(
                [state = cancellation_wake_state()] {
                state->request_cancel();
            });
        }

        ~cancellable_event_waiter() {
            cancel_registration_.unregister();
        }

        coro::cancel_result await_resume_cancellable() noexcept {
            cancel_registration_.unregister();
            return await_resume_impl();
        }

    private:
        coro::cancel_token::registration cancel_registration_;
    };

    class wait_awaitable {
    public:
        explicit wait_awaitable(event& e) : waiter_(e) {}

        bool await_ready() const noexcept { return waiter_.await_ready_impl(); }
        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            return waiter_.await_suspend_impl(awaiter);
        }
        void await_resume() const noexcept {
            (void)waiter_.await_resume_impl();
        }

    private:
        mutable event_waiter waiter_;
    };

    class cancellable_wait_awaitable {
    public:
        cancellable_wait_awaitable(event& e, coro::cancel_token token)
            : waiter_(e, std::move(token)) {}

        bool await_ready() const noexcept { return waiter_.await_ready_impl(); }
        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            return waiter_.await_suspend_impl(awaiter);
        }
        [[nodiscard("check whether the event wait completed")]]
        coro::cancel_result await_resume() noexcept {
            return waiter_.await_resume_cancellable();
        }

    private:
        cancellable_event_waiter waiter_;
    };

    /// Wait for the event to be signaled.
    auto wait() {
        return wait_awaitable(*this);
    }

    /// Wait for the event, or return cancelled if the token wins the race.
    auto wait(coro::cancel_token token) {
        return cancellable_wait_awaitable(*this, std::move(token));
    }

    /// Signal the event (wake all waiters)
    void set() {
        std::vector<detail::wake_state_ptr> to_schedule;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            signaled_.store(true, std::memory_order_release);

            // Collect handles and pop from list under lock.
            // Popping marks nodes as unlinked, so destructors won't try to remove them.
            while (!waiters_.empty()) {
                auto* waiter = waiters_.pop_front();
                if (waiter->cancellable_) {
                    if (detail::claim_wake_state(waiter->wake_state_) ==
                        detail::wake_action::rejected) {
                        continue;
                    }
                }
                to_schedule.push_back(waiter->wake_state_);
            }
        }
        // Schedule outside lock to avoid deadlock if schedule_handle()
        // resumes inline (trampoline path) and destructor re-acquires mutex.
        detail::schedule_wake_states(to_schedule);
    }

    /// Reset the event
    void reset() noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        signaled_.store(false, std::memory_order_release);
    }

    /// Check if signaled
    bool is_set() const noexcept {
        return signaled_.load(std::memory_order_acquire);
    }

private:
    std::mutex mutex_;
    std::atomic<bool> signaled_{false};
    elio::detail::intrusive_list<event_waiter> waiters_;
};

} // namespace elio::sync
