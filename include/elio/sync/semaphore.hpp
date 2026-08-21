#pragma once

#include <cassert>
#include <climits>
#include <coroutine>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <vector>
#include <algorithm>
#include <utility>
#include "../coro/cancel_token.hpp"
#include "../detail/intrusive_list.hpp"
#include "../runtime/scheduler.hpp"
#include "detail/wake_state.hpp"

namespace elio::sync {

namespace detail {
#ifdef ELIO_RUNTIME_TEST_HOOKS
inline std::atomic<size_t> semaphore_waiter_publications_for_test{0};
inline std::atomic<bool>
    pause_semaphore_cancellable_ready_before_claim_for_test{false};
inline std::atomic<bool>
    semaphore_cancellable_ready_paused_before_claim_for_test{false};

inline void pause_semaphore_cancellable_ready_for_test() noexcept {
    auto& pause = pause_semaphore_cancellable_ready_before_claim_for_test;
    if (!pause.load(std::memory_order_acquire)) return;

    semaphore_cancellable_ready_paused_before_claim_for_test.store(
        true, std::memory_order_release);
    semaphore_cancellable_ready_paused_before_claim_for_test.notify_all();
    while (pause.load(std::memory_order_acquire)) {
        pause.wait(true, std::memory_order_acquire);
    }
    semaphore_cancellable_ready_paused_before_claim_for_test.store(
        false, std::memory_order_release);
    semaphore_cancellable_ready_paused_before_claim_for_test.notify_all();
}
#endif
}

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
            : sem_(s) {}

        acquire_waiter(semaphore& s, bool cancellable)
            : sem_(s)
            , waiter_state_(cancellable) {}

        ~acquire_waiter() {
            // Fast path: if we never suspended, we were never enqueued,
            // so no wake function could hold a reference to us.
            if (!waiter_state_.suspended) return;

            detail::wake_state_ptr to_schedule;
            // Slow path: acquire mutex to prevent race with release()
            {
                std::lock_guard<std::mutex> guard(sem_.mutex_);
                if (this->is_linked()) {
                    sem_.waiters_.remove(this);
                    detail::cancel_wake_state(waiter_state_.wake());
                } else if (waiter_state_.grant_pending &&
                           !waiter_state_.resumed) {
                    detail::cancel_wake_state(waiter_state_.wake());
                    waiter_state_.grant_pending = false;
                    to_schedule = sem_.recover_cancelled_handoff_locked();
                } else {
                    detail::cancel_wake_state(waiter_state_.wake());
                }
            }

            if (to_schedule) {
                detail::schedule_wake_state(to_schedule);
            }
        }

        bool await_ready_impl() const {
            if (!waiter_state_.cancellable) {
                return sem_.try_acquire();
            }

            if (waiter_state_->was_cancelled()) {
                return true;
            }

            std::lock_guard<std::mutex> guard(sem_.mutex_);
            if (sem_.count_ == 0) {
                return false;
            }
#ifdef ELIO_RUNTIME_TEST_HOOKS
            detail::pause_semaphore_cancellable_ready_for_test();
#endif
            // Select completion before exposing a permit debit to release().
            const auto action =
                detail::claim_wake_state(waiter_state_.wake());
            if (action == detail::wake_action::rejected) {
                return true;
            }

            assert(action == detail::wake_action::completed_inline);
            --sem_.count_;
            return true;
        }

        bool await_suspend_impl(std::coroutine_handle<> awaiter) {
            if (waiter_state_.cancellable) {
                return await_suspend_cancellable_impl(awaiter);
            }
            return await_suspend_non_cancellable_impl(awaiter);
        }

        bool await_suspend_non_cancellable_impl(
                std::coroutine_handle<> awaiter) {
            assert(!waiter_state_.cancellable);

            // A release may outlive the coroutine frame after dequeuing this
            // waiter, so an acquire that reaches suspension still needs
            // independent shared ownership. Allocate before taking the queue
            // lock so failure leaves both the permit count and queue unchanged.
            waiter_state_.emplace();

            std::lock_guard<std::mutex> guard(sem_.mutex_);
            if (sem_.count_ > 0) {
                --sem_.count_;
                return false;
            }

            waiter_state_->set_handle(awaiter);
            sem_.waiters_.push_back(this);
            waiter_state_.suspended = true;
#ifdef ELIO_RUNTIME_TEST_HOOKS
            detail::semaphore_waiter_publications_for_test.fetch_add(
                1, std::memory_order_release);
#endif
            return true;
        }

        bool await_suspend_cancellable_impl(
                std::coroutine_handle<> awaiter) noexcept {
            assert(waiter_state_.cancellable);
            detail::wake_state_ptr to_schedule;
            {
                std::lock_guard<std::mutex> guard(sem_.mutex_);

                if (waiter_state_->was_cancelled()) {
                    return false;
                }

                if (sem_.count_ > 0) {
                    --sem_.count_;
                    if (detail::claim_wake_state(waiter_state_.wake()) !=
                        detail::wake_action::rejected) {
                        return false;
                    }

                    // Cancellation won the acquire race. Hand the permit to
                    // another live waiter, or restore it to the count.
                    to_schedule = sem_.recover_cancelled_handoff_locked();
                } else {
                    if (!waiter_state_->set_handle_blocked(awaiter)) {
                        return false;
                    }
                    sem_.waiters_.push_back(this);
                    waiter_state_.suspended = true;
#ifdef ELIO_RUNTIME_TEST_HOOKS
                    detail::semaphore_waiter_publications_for_test.fetch_add(
                        1, std::memory_order_release);
#endif

                    if (waiter_state_->unblock_after_publish()) {
                        return true;
                    }

                    sem_.waiters_.remove(this);
                    waiter_state_.suspended = false;
                }
            }

            if (to_schedule) {
                detail::schedule_wake_state(to_schedule);
            }
            return false;
        }

        coro::cancel_result await_resume_impl() noexcept {
            if (!waiter_state_.cancellable) {
                waiter_state_.resumed = true;
                waiter_state_.grant_pending = false;
                waiter_state_.suspended = false;
                return coro::cancel_result::completed;
            }

            if (waiter_state_->was_cancelled()) {
                if (waiter_state_.suspended) {
                    std::lock_guard<std::mutex> guard(sem_.mutex_);
                    if (this->is_linked()) {
                        sem_.waiters_.remove(this);
                    }
                    waiter_state_.suspended = false;
                }
                return coro::cancel_result::cancelled;
            }

            waiter_state_.resumed = true;
            waiter_state_.grant_pending = false;
            waiter_state_.suspended = false;
            return coro::cancel_result::completed;
        }

    protected:
        const detail::wake_state_ptr& cancellation_wake_state() const noexcept {
            return waiter_state_.wake();
        }

    private:
        class waiter_state {
        public:
            waiter_state() noexcept = default;

            explicit waiter_state(bool is_cancellable)
                : cancellable(is_cancellable) {
                if (!is_cancellable) return;

                ::new (static_cast<void*>(storage_))
                    detail::wake_state_ptr(detail::make_wake_state());
                // Publish engagement only after placement construction
                // succeeds; a throwing allocation leaves no active object.
                engaged = true;
            }

            ~waiter_state() {
                if (engaged) {
                    std::destroy_at(std::addressof(storage_ref()));
                }
            }

            waiter_state(const waiter_state&) = delete;
            waiter_state& operator=(const waiter_state&) = delete;
            waiter_state(waiter_state&&) = delete;
            waiter_state& operator=(waiter_state&&) = delete;

            void emplace() {
                assert(!engaged);
                ::new (static_cast<void*>(storage_))
                    detail::wake_state_ptr(detail::make_wake_state());
                // A failed allocation leaves the slot disengaged.
                engaged = true;
            }

            [[nodiscard]] const detail::wake_state_ptr& wake() const noexcept {
                assert(engaged);
                return storage_ref();
            }

            [[nodiscard]] detail::wake_state* operator->() const noexcept {
                return wake().get();
            }

            bool engaged = false;
            bool cancellable = false;
            bool suspended = false;
            bool resumed = false;
            bool grant_pending = false;

        private:
            detail::wake_state_ptr& storage_ref() noexcept {
                return *std::launder(storage_ptr());
            }

            const detail::wake_state_ptr& storage_ref() const noexcept {
                return *std::launder(storage_ptr());
            }

            detail::wake_state_ptr* storage_ptr() noexcept {
                return reinterpret_cast<detail::wake_state_ptr*>(storage_);
            }

            const detail::wake_state_ptr* storage_ptr() const noexcept {
                return reinterpret_cast<const detail::wake_state_ptr*>(
                    storage_);
            }

            alignas(detail::wake_state_ptr)
                std::byte storage_[sizeof(detail::wake_state_ptr)];
        };

        semaphore& sem_;
        waiter_state waiter_state_;

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

        bool await_suspend_impl(
                std::coroutine_handle<> awaiter) noexcept {
            return await_suspend_cancellable_impl(awaiter);
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
        bool await_suspend(std::coroutine_handle<> awaiter) {
            return waiter_.await_suspend_non_cancellable_impl(awaiter);
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

    /// Acquire (decrement) the semaphore. A ready no-token acquire completes
    /// without allocation. Suspension allocates shared wake state and can
    /// propagate std::bad_alloc from await_suspend().
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

#ifdef ELIO_RUNTIME_TEST_HOOKS
    bool try_lock_mutex_for_test() noexcept {
        if (!mutex_.try_lock()) return false;
        mutex_.unlock();
        return true;
    }
#endif

private:
    mutable std::mutex mutex_;
    int count_;
    elio::detail::intrusive_list<acquire_waiter> waiters_;

    detail::wake_state_ptr claim_waiter_locked() noexcept {
        while (!waiters_.empty()) {
            auto* waiter = waiters_.pop_front();
            if (waiter->waiter_state_.cancellable) {
                if (detail::claim_wake_state(waiter->waiter_state_.wake()) ==
                    detail::wake_action::rejected) {
                    continue;
                }
            }
            waiter->waiter_state_.grant_pending = true;
            return waiter->waiter_state_.wake();
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

};

} // namespace elio::sync
