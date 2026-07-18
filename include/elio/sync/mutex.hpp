#pragma once

#include <coroutine>
#include <atomic>
#include <mutex>
#include <cassert>
#include <utility>
#include "../coro/cancel_token.hpp"
#include "../detail/intrusive_list.hpp"
#include "../runtime/scheduler.hpp"
#include "detail/wake_state.hpp"

namespace elio::sync {

class mutex {
public:
    // Forward declarations for the shared intrusive waiter node and public
    // awaiters.
    class lock_waiter;
    class cancellable_lock_waiter;
    class lock_awaitable;
    class cancellable_lock_awaitable;

    mutex() = default;

    ~mutex() {
        assert(waiters_.empty() && "mutex destroyed with pending waiters");
    }

    // Non-copyable, non-movable
    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;
    mutex(mutex&&) = delete;
    mutex& operator=(mutex&&) = delete;

    class lock_waiter : public elio::detail::intrusive_list_node<lock_waiter> {
    public:
        explicit lock_waiter(mutex& m)
            : mtx_(m)
            , wake_state_(detail::make_wake_state()) {}

        lock_waiter(mutex& m, bool cancellable)
            : mtx_(m)
            , wake_state_(detail::make_wake_state())
            , cancellable_(cancellable) {}

        ~lock_waiter() {
            // Fast path: if we never suspended, we were never enqueued,
            // so no wake function could hold a reference to us.
            if (!suspended_) return;

            detail::wake_state_ptr to_schedule;
            // Slow path: acquire internal_mutex_ to prevent race with unlock()
            {
                std::lock_guard<std::mutex> guard(mtx_.internal_mutex_);
                if (this->is_linked()) {
                    mtx_.waiters_.remove(this);
                    detail::cancel_wake_state(wake_state_);
                } else if (grant_pending_ && !resumed_) {
                    detail::cancel_wake_state(wake_state_);
                    grant_pending_ = false;
                    to_schedule = mtx_.recover_cancelled_handoff_locked();
                } else {
                    detail::cancel_wake_state(wake_state_);
                }
            }

            if (to_schedule) {
                detail::schedule_wake_state(to_schedule);
            }
        }

        bool await_ready_impl() const noexcept {
            if (!cancellable_) {
                return mtx_.try_lock();
            }

            if (wake_state_->was_cancelled()) {
                return true;
            }
            if (!mtx_.try_lock()) {
                return false;
            }
            if (detail::claim_wake_state(wake_state_) !=
                detail::wake_action::rejected) {
                return true;
            }

            // Cancellation won after the lock was acquired.
            mtx_.unlock();
            return true;
        }

        bool await_suspend_impl(std::coroutine_handle<> awaiter) noexcept {
            if (!cancellable_) {
                void* expected = nullptr;
                if (mtx_.state_.compare_exchange_strong(
                        expected, awaiter.address(),
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    return false;
                }

                std::lock_guard<std::mutex> guard(mtx_.internal_mutex_);
                expected = nullptr;
                if (mtx_.state_.compare_exchange_strong(
                        expected, awaiter.address(),
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    return false;
                }

                wake_state_->set_handle(awaiter);
                mtx_.waiters_.push_back(this);
                suspended_ = true;
                return true;
            }

            detail::wake_state_ptr to_schedule;
            {
                // Lock is held, add to wait queue
                std::lock_guard<std::mutex> guard(mtx_.internal_mutex_);

                if (wake_state_->was_cancelled()) {
                    return false;
                }

                // Double-check after acquiring internal lock
                void* expected = nullptr;
                if (mtx_.state_.compare_exchange_strong(
                        expected, awaiter.address(),
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    if (detail::claim_wake_state(wake_state_) !=
                        detail::wake_action::rejected) {
                        return false;
                    }

                    // Cancellation won the acquire race. Transfer the lock to
                    // another live waiter, or release it when none remain.
                    to_schedule = mtx_.recover_cancelled_handoff_locked();
                } else {
                    if (!wake_state_->set_handle_blocked(awaiter)) {
                        return false;
                    }
                    mtx_.waiters_.push_back(this);
                    suspended_ = true;

                    if (wake_state_->unblock_after_publish()) {
                        return true;
                    }

                    mtx_.waiters_.remove(this);
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
                    std::lock_guard<std::mutex> guard(mtx_.internal_mutex_);
                    if (this->is_linked()) {
                        mtx_.waiters_.remove(this);
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
        mutex& mtx_;
        detail::wake_state_ptr wake_state_;
        bool cancellable_ = false;
        bool suspended_ = false;  // True if enqueued in waiters_
        bool resumed_ = false;    // True after a popped waiter resumes normally
        bool grant_pending_ = false;  // True after unlock() transfers ownership

        friend class mutex;
    };

    class cancellable_lock_waiter : public lock_waiter {
    public:
        cancellable_lock_waiter(mutex& m, coro::cancel_token token)
            : lock_waiter(m, true) {
            cancel_registration_ = token.on_cancel(
                [state = cancellation_wake_state()] {
                state->request_cancel();
            });
        }

        ~cancellable_lock_waiter() {
            cancel_registration_.unregister();
        }

        coro::cancel_result await_resume_cancellable() noexcept {
            cancel_registration_.unregister();
            return await_resume_impl();
        }

    private:
        coro::cancel_token::registration cancel_registration_;
    };

    /// Non-cancellable lock awaiter. The await result remains void for source
    /// compatibility with the pre-0.6 API.
    class lock_awaitable {
    public:
        explicit lock_awaitable(mutex& m) : waiter_(m) {}

        bool await_ready() const noexcept { return waiter_.await_ready_impl(); }
        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            return waiter_.await_suspend_impl(awaiter);
        }
        void await_resume() noexcept {
            (void)waiter_.await_resume_impl();
        }

    private:
        lock_waiter waiter_;
    };

    /// Cancellation-aware lock awaiter. Callers must check the result before
    /// entering the protected critical section.
    class cancellable_lock_awaitable {
    public:
        cancellable_lock_awaitable(mutex& m, coro::cancel_token token)
            : waiter_(m, std::move(token)) {}

        bool await_ready() const noexcept { return waiter_.await_ready_impl(); }
        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            return waiter_.await_suspend_impl(awaiter);
        }
        [[nodiscard("check whether the mutex was acquired")]]
        coro::cancel_result await_resume() noexcept {
            return waiter_.await_resume_cancellable();
        }

    private:
        cancellable_lock_waiter waiter_;
    };

    /// Unlock awaitable — releases the lock and wakes one waiter if any
    class unlock_awaitable {
    public:
        explicit unlock_awaitable(mutex& m) : mtx_(m) {}

        bool await_ready() const noexcept {
            return true; // Always ready
        }

        bool await_suspend(std::coroutine_handle<>) const noexcept {
            return false; // Never suspend
        }

        void await_resume() const noexcept {
            mtx_.unlock();
        }

    private:
        mutex& mtx_;
    };

    /// Lock the mutex (coroutine-aware).
    [[nodiscard]] lock_awaitable lock() {
        return lock_awaitable(*this);
    }

    /// Lock the mutex, or return cancelled if the token wins the wait race.
    [[nodiscard]] cancellable_lock_awaitable lock(coro::cancel_token token) {
        return cancellable_lock_awaitable(*this, std::move(token));
    }

    /// Try to lock the mutex (non-blocking)
    bool try_lock() noexcept {
        void* expected = nullptr;
        return state_.compare_exchange_strong(
            expected, reinterpret_cast<void*>(1),
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    /// Unlock the mutex and wake one waiter if any
    void unlock() noexcept {
        detail::wake_state_ptr to_schedule;
        {
            std::lock_guard<std::mutex> guard(internal_mutex_);

            to_schedule = recover_cancelled_handoff_locked();
        }
        // Schedule outside lock to avoid deadlock if schedule_handle()
        // resumes inline (trampoline path) and destructor re-acquires mutex.
        if (to_schedule) {
            detail::schedule_wake_state(to_schedule);
        }
    }

    /// Check if the mutex is locked
    bool is_locked() const noexcept {
        return state_.load(std::memory_order_acquire) != nullptr;
    }

private:
    std::atomic<void*> state_{nullptr};
    mutable std::mutex internal_mutex_;
    elio::detail::intrusive_list<lock_waiter> waiters_;

    detail::wake_state_ptr recover_cancelled_handoff_locked() noexcept {
        while (!waiters_.empty()) {
            auto* waiter = waiters_.pop_front();
            if (waiter->cancellable_) {
                const auto action =
                    detail::claim_wake_state(waiter->wake_state_);
                if (action == detail::wake_action::rejected) {
                    continue;
                }
            }

            state_.store(reinterpret_cast<void*>(1), std::memory_order_release);
            waiter->grant_pending_ = true;
            return waiter->wake_state_;
        }

        state_.store(nullptr, std::memory_order_release);
        return nullptr;
    }
};

/// RAII lock guard for mutex (synchronous)
/// Note: This guard assumes the mutex is already locked when constructed.
/// For coroutine-aware locking, use: co_await m.lock();
class lock_guard {
public:
    explicit lock_guard(mutex& m) : mutex_(&m), owns_lock_(true) {}

    ~lock_guard() {
        if (owns_lock_) {
            mutex_->unlock();
        }
    }

    // Non-copyable, movable
    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;

    lock_guard(lock_guard&& other) noexcept
        : mutex_(other.mutex_), owns_lock_(other.owns_lock_) {
        other.owns_lock_ = false;
    }

    lock_guard& operator=(lock_guard&& other) noexcept {
        if (this != &other) {
            if (owns_lock_) {
                mutex_->unlock();
            }
            mutex_ = other.mutex_;
            owns_lock_ = other.owns_lock_;
            other.owns_lock_ = false;
        }
        return *this;
    }

    void unlock() {
        if (owns_lock_) {
            mutex_->unlock();
            owns_lock_ = false;
        }
    }

private:
    mutex* mutex_;
    bool owns_lock_;
};

} // namespace elio::sync
