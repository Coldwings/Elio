#pragma once

#include <coroutine>
#include <mutex>
#include <vector>
#include <concepts>
#include <cassert>
#include <utility>
#include "../coro/cancel_token.hpp"
#include "../detail/intrusive_list.hpp"
#include "../runtime/scheduler.hpp"
#include "detail/wake_state.hpp"
#include "mutex.hpp"

namespace elio::sync {

/// Lock concept for condition_variable
namespace detail {

template<typename Lock>
concept lockable = requires(Lock& l) {
    l.unlock();
    l.lock();
};

} // namespace detail

/// Coroutine-aware condition variable
///
/// Suspends the coroutine instead of blocking the thread.
/// Supports three modes of use:
/// - With elio::sync::mutex (coroutine-aware async re-lock)
/// - With elio::sync::spinlock or any lockable type (synchronous re-lock)
/// - Without any lock (wait_unlocked) for single-worker scenarios
///
/// IMPORTANT: Always use a predicate loop to protect against spurious wakeups:
/// @code
/// co_await mtx.lock();
/// while (!condition) {
///     co_await cv.wait(mtx);
/// }
/// mtx.unlock();
/// @endcode
class condition_variable {
public:
    /// Common base for all cv waiter types.
    /// Inherits intrusive_list_node so all awaiter types can share one list.
    class cv_waiter_base : public elio::detail::intrusive_list_node<cv_waiter_base> {
    public:
        ~cv_waiter_base() {
            if (!suspended_) return;

            std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
            if (this->is_linked()) {
                cv_.waiters_.remove(this);
            }
            detail::cancel_wake_state(wake_state_);
        }

        detail::wake_state_ptr wake_state_;
        bool cancellable_ = false;
        bool suspended_ = false;  // True if enqueued in waiters_

    protected:
        explicit cv_waiter_base(condition_variable& cv,
                                bool cancellable = false)
            : wake_state_(detail::make_wake_state())
            , cancellable_(cancellable)
            , cv_(cv) {}

        [[nodiscard]] bool prepare_blocked_wait(
                std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
            if (cancellable_ && wake_state_->was_cancelled()) {
                return false;
            }
            if (!wake_state_->set_handle_blocked(awaiter)) {
                return false;
            }
            cv_.waiters_.push_back(this);
            suspended_ = true;
            return true;
        }

        [[nodiscard]] bool finish_blocked_publication() noexcept {
            if (wake_state_->unblock_after_publish()) {
                return true;
            }

            std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
            if (this->is_linked()) {
                cv_.waiters_.remove(this);
            }
            suspended_ = false;
            return false;
        }

        coro::cancel_result finish_wait() noexcept {
            if (cancellable_ && wake_state_->was_cancelled()) {
                if (suspended_) {
                    std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
                    if (this->is_linked()) {
                        cv_.waiters_.remove(this);
                    }
                }
                suspended_ = false;
                return coro::cancel_result::cancelled;
            }

            suspended_ = false;
            return coro::cancel_result::completed;
        }

        const detail::wake_state_ptr& cancellation_wake_state() const noexcept {
            return wake_state_;
        }

    private:
        condition_variable& cv_;
    };

    condition_variable() = default;

    ~condition_variable() {
        assert(waiters_.empty() && "condition_variable destroyed with pending waiters");
    }

    // Non-copyable, non-movable
    condition_variable(const condition_variable&) = delete;
    condition_variable& operator=(const condition_variable&) = delete;
    condition_variable(condition_variable&&) = delete;
    condition_variable& operator=(condition_variable&&) = delete;

    /// Internal awaitable: suspends until notified, does NOT re-lock.
    class wait_suspend_awaitable : public cv_waiter_base {
    public:
        wait_suspend_awaitable(condition_variable& cv, mutex& m)
            : cv_waiter_base(cv), cv_(cv), mutex_(m) {}

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            {
                std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
                this->wake_state_->set_handle_blocked(awaiter);
                cv_.waiters_.push_back(this);
                this->suspended_ = true;  // Mark as enqueued
            }
            // The waiter is visible before unlocking the user mutex to avoid
            // lost wakeups, but its wake_state is blocked so notify_* cannot
            // resume and destroy this awaiter until after unlock() returns.
            mutex_.unlock();
            return this->wake_state_->unblock_after_publish();
        }

        void await_resume() const noexcept {}

    private:
        condition_variable& cv_;
        mutex& mutex_;
    };

    /// Wait awaitable for use with a generic lockable type (e.g., spinlock)
    template<detail::lockable Lock>
    class wait_awaitable_lock : public cv_waiter_base {
    public:
        wait_awaitable_lock(condition_variable& cv, Lock& lock)
            : cv_waiter_base(cv), cv_(cv), lock_(lock) {}

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            {
                std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
                this->wake_state_->set_handle_blocked(awaiter);
                cv_.waiters_.push_back(this);
                this->suspended_ = true;  // Mark as enqueued
            }
            // The waiter is visible before unlocking the user lock to avoid
            // lost wakeups, but its wake_state is blocked so notify_* cannot
            // resume and destroy this awaiter until after unlock() returns.
            lock_.unlock();
            return this->wake_state_->unblock_after_publish();
        }

        void await_resume() {
            lock_.lock();
        }

    private:
        condition_variable& cv_;
        Lock& lock_;
    };

    /// Wait awaitable without any external lock
    class wait_awaitable_unlocked : public cv_waiter_base {
    public:
        explicit wait_awaitable_unlocked(condition_variable& cv)
            : cv_waiter_base(cv), cv_(cv) {}

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
            this->wake_state_->set_handle(awaiter);
            cv_.waiters_.push_back(this);
            this->suspended_ = true;  // Mark as enqueued
            return true;
        }

        void await_resume() const noexcept {}

    private:
        condition_variable& cv_;
    };

    struct cancellable_wait_completion {
        coro::cancel_result result;
        bool lock_released;
    };

    /// Internal cancellable wait for elio::sync::mutex. Re-locking remains
    /// asynchronous and is completed by wait(mutex&, cancel_token).
    class cancellable_wait_suspend_awaitable : public cv_waiter_base {
    public:
        cancellable_wait_suspend_awaitable(condition_variable& cv, mutex& m,
                                           coro::cancel_token token)
            : cv_waiter_base(cv, true)
            , mutex_(m) {
            cancel_registration_ = token.on_cancel(
                [state = cancellation_wake_state()] {
                state->request_cancel();
            });
        }

        ~cancellable_wait_suspend_awaitable() {
            cancel_registration_.unregister();
        }

        bool await_ready() const noexcept {
            return cancellation_wake_state()->was_cancelled();
        }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            if (!prepare_blocked_wait(awaiter)) {
                return false;
            }

            mutex_.unlock();
            lock_released_ = true;
            return finish_blocked_publication();
        }

        cancellable_wait_completion await_resume() noexcept {
            cancel_registration_.unregister();
            return {finish_wait(), lock_released_};
        }

    private:
        mutex& mutex_;
        coro::cancel_token::registration cancel_registration_;
        bool lock_released_ = false;
    };

    /// Cancellable wait for a synchronous lockable type such as spinlock.
    template<detail::lockable Lock>
    class cancellable_wait_awaitable_lock : public cv_waiter_base {
    public:
        cancellable_wait_awaitable_lock(condition_variable& cv, Lock& lock,
                                        coro::cancel_token token)
            : cv_waiter_base(cv, true)
            , lock_(lock) {
            cancel_registration_ = token.on_cancel(
                [state = this->cancellation_wake_state()] {
                state->request_cancel();
            });
        }

        ~cancellable_wait_awaitable_lock() {
            cancel_registration_.unregister();
        }

        bool await_ready() const noexcept {
            return this->cancellation_wake_state()->was_cancelled();
        }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            if (!this->prepare_blocked_wait(awaiter)) {
                return false;
            }

            lock_.unlock();
            lock_released_ = true;
            return this->finish_blocked_publication();
        }

        [[nodiscard("check whether the condition wait was notified")]]
        coro::cancel_result await_resume() {
            cancel_registration_.unregister();
            const auto result = this->finish_wait();
            if (lock_released_) {
                lock_.lock();
            }
            return result;
        }

    private:
        Lock& lock_;
        coro::cancel_token::registration cancel_registration_;
        bool lock_released_ = false;
    };

    /// Cancellable wait without an external lock.
    class cancellable_wait_awaitable_unlocked : public cv_waiter_base {
    public:
        cancellable_wait_awaitable_unlocked(condition_variable& cv,
                                            coro::cancel_token token)
            : cv_waiter_base(cv, true) {
            cancel_registration_ = token.on_cancel(
                [state = cancellation_wake_state()] {
                state->request_cancel();
            });
        }

        ~cancellable_wait_awaitable_unlocked() {
            cancel_registration_.unregister();
        }

        bool await_ready() const noexcept {
            return cancellation_wake_state()->was_cancelled();
        }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            if (!prepare_blocked_wait(awaiter)) {
                return false;
            }
            return finish_blocked_publication();
        }

        [[nodiscard("check whether the condition wait was notified")]]
        coro::cancel_result await_resume() noexcept {
            cancel_registration_.unregister();
            return finish_wait();
        }

    private:
        coro::cancel_token::registration cancel_registration_;
    };

    /// Wait with elio::sync::mutex
    ///
    /// Atomically releases the mutex and suspends the coroutine.
    /// When notified, re-acquires the mutex before returning.
    ///
    /// Usage:
    /// @code
    /// co_await mtx.lock();
    /// while (!condition) {
    ///     co_await cv.wait(mtx);
    /// }
    /// mtx.unlock();
    /// @endcode
    coro::task<void> wait(mutex& m) {
        // Atomically suspend and release mutex. When we resume, we must
        // re-acquire the mutex before returning. The wait_suspend_awaitable
        // handles the suspend+release, and we re-lock after resuming.
        co_await wait_suspend_awaitable(*this, m);
        // Re-acquire mutex after resuming to maintain CV contract
        co_await m.lock();
    }

    /// Wait with elio::sync::mutex until notified or cancellation wins.
    /// The mutex is held again before returning whenever the wait released it.
    coro::task<coro::cancel_result> wait(mutex& m,
                                         coro::cancel_token token) {
        const auto completion = co_await cancellable_wait_suspend_awaitable(
            *this, m, std::move(token));
        if (completion.lock_released) {
            co_await m.lock();
        }
        co_return completion.result;
    }

    /// Wait with a generic lockable (e.g., spinlock)
    template<detail::lockable Lock>
    auto wait(Lock& lock) {
        return wait_awaitable_lock<Lock>(*this, lock);
    }

    /// Wait with a synchronous lockable until notified or cancellation wins.
    template<detail::lockable Lock>
    auto wait(Lock& lock, coro::cancel_token token) {
        return cancellable_wait_awaitable_lock<Lock>(
            *this, lock, std::move(token));
    }

    /// Wait without external lock (single-worker only)
    auto wait_unlocked() {
        return wait_awaitable_unlocked(*this);
    }

    /// Wait without an external lock until notified or cancellation wins.
    auto wait_unlocked(coro::cancel_token token) {
        return cancellable_wait_awaitable_unlocked(
            *this, std::move(token));
    }

    /// Wake one waiting coroutine
    void notify_one() {
        detail::wake_state_ptr to_schedule;
        {
            std::lock_guard<std::mutex> guard(internal_mutex_);
            while (!waiters_.empty()) {
                // Popping marks the node as unlinked. A cancellation winner is
                // skipped so notify_one still reaches one eligible waiter.
                auto* waiter = waiters_.pop_front();
                if (waiter->cancellable_ &&
                    detail::claim_wake_state(waiter->wake_state_) ==
                        detail::wake_action::rejected) {
                    continue;
                }
                to_schedule = waiter->wake_state_;
                break;
            }
        }
        // Schedule outside lock to avoid deadlock if schedule_handle()
        // resumes inline (trampoline path) and destructor re-acquires mutex.
        detail::schedule_wake_state(to_schedule);
    }

    /// Wake all waiting coroutines
    void notify_all() {
        std::vector<detail::wake_state_ptr> to_schedule;
        {
            std::lock_guard<std::mutex> guard(internal_mutex_);
            to_schedule.reserve(waiters_.size());
            while (!waiters_.empty()) {
                // Collect handles and pop from list under lock.
                // Popping marks nodes as unlinked, so destructors won't try to remove them.
                auto* waiter = waiters_.pop_front();
                if (waiter->cancellable_ &&
                    detail::claim_wake_state(waiter->wake_state_) ==
                        detail::wake_action::rejected) {
                    continue;
                }
                to_schedule.push_back(waiter->wake_state_);
            }
        }
        // Schedule outside lock to avoid deadlock if schedule_handle()
        // resumes inline (trampoline path) and destructor re-acquires mutex.
        detail::schedule_wake_states(to_schedule);
    }

    /// Check if there are waiting coroutines
    bool has_waiters() const noexcept {
        std::lock_guard<std::mutex> guard(internal_mutex_);
        return !waiters_.empty();
    }

private:
    mutable std::mutex internal_mutex_;
    elio::detail::intrusive_list<cv_waiter_base> waiters_;
};

} // namespace elio::sync
