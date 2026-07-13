#pragma once

#include <coroutine>
#include <mutex>
#include <vector>
#include <concepts>
#include <cassert>
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
        detail::wake_state_ptr wake_state_;
        bool suspended_ = false;  // True if enqueued in waiters_
    protected:
        cv_waiter_base()
            : wake_state_(detail::make_wake_state()) {}
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
            : cv_(cv), mutex_(m) {}

        ~wait_suspend_awaitable() {
            // Fast path: if we never suspended, we were never enqueued,
            // so no wake function could hold a reference to us.
            if (!this->suspended_) return;

            // Slow path: acquire internal_mutex_ to prevent race with notify
            std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
            if (this->is_linked()) {
                cv_.waiters_.remove(this);
            }
            detail::cancel_wake_state(this->wake_state_);
        }

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
            : cv_(cv), lock_(lock) {}

        ~wait_awaitable_lock() {
            // Fast path: if we never suspended, we were never enqueued
            if (!this->suspended_) return;

            // Slow path: acquire internal_mutex_ to prevent race with notify
            std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
            if (this->is_linked()) {
                cv_.waiters_.remove(this);
            }
            detail::cancel_wake_state(this->wake_state_);
        }

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
        explicit wait_awaitable_unlocked(condition_variable& cv) : cv_(cv) {}

        ~wait_awaitable_unlocked() {
            // Fast path: if we never suspended, we were never enqueued
            if (!this->suspended_) return;

            // Slow path: acquire internal_mutex_ to prevent race with notify
            std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
            if (this->is_linked()) {
                cv_.waiters_.remove(this);
            }
            detail::cancel_wake_state(this->wake_state_);
        }

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

    /// Wait with a generic lockable (e.g., spinlock)
    template<detail::lockable Lock>
    auto wait(Lock& lock) {
        return wait_awaitable_lock<Lock>(*this, lock);
    }

    /// Wait without external lock (single-worker only)
    auto wait_unlocked() {
        return wait_awaitable_unlocked(*this);
    }

    /// Wake one waiting coroutine
    void notify_one() {
        detail::wake_state_ptr to_schedule;
        {
            std::lock_guard<std::mutex> guard(internal_mutex_);
            if (waiters_.empty()) return;

            // Collect handle and pop from list under lock.
            // Popping marks node as unlinked, so destructor's locked slow path
            // won't try to remove it (is_linked() == false).
            auto* waiter = waiters_.pop_front();
            to_schedule = waiter->wake_state_;
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
            while (!waiters_.empty()) {
                // Collect handles and pop from list under lock.
                // Popping marks nodes as unlinked, so destructors won't try to remove them.
                auto* waiter = waiters_.pop_front();
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
