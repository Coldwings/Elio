#pragma once

#include <cassert>
#include <climits>
#include <coroutine>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include "../detail/intrusive_list.hpp"
#include "../runtime/scheduler.hpp"
#include "detail/wake_state.hpp"

namespace elio::sync {

/// Coroutine-aware semaphore
class semaphore {
public:
    // Forward declaration for intrusive_list
    class acquire_awaitable;

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

    /// Acquire awaitable — inherits intrusive_list_node for safe unlinking
    class acquire_awaitable : public elio::detail::intrusive_list_node<acquire_awaitable> {
    public:
        explicit acquire_awaitable(semaphore& s)
            : sem_(s)
            , wake_state_(detail::make_wake_state()) {}

        ~acquire_awaitable() {
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

        bool await_ready() const noexcept {
            return sem_.try_acquire();
        }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(sem_.mutex_);

            if (sem_.count_ > 0) {
                --sem_.count_;
                return false;  // Don't suspend
            }

            wake_state_->set_handle(awaiter);
            sem_.waiters_.push_back(this);
            suspended_ = true;  // Mark as enqueued
            return true;  // Suspend
        }

        void await_resume() noexcept {
            resumed_ = true;
            grant_pending_ = false;
        }

    private:
        semaphore& sem_;
        detail::wake_state_ptr wake_state_;
        bool suspended_ = false;  // True if enqueued in waiters_
        bool resumed_ = false;    // True after a popped waiter resumes normally
        bool grant_pending_ = false;  // True after release() transfers a permit

        friend class semaphore;
    };

    /// Acquire (decrement) the semaphore
    auto acquire() {
        return acquire_awaitable(*this);
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

            // Calculate how many waiters to wake (up to 'count')
            const int to_wake = std::min(count, static_cast<int>(waiters_.size()));

            // Collect handles and pop from list under lock.
            // Popping marks nodes as unlinked, so destructors won't try to remove them.
            for (int i = 0; i < to_wake; ++i) {
                auto* waiter = waiters_.pop_front();
                waiter->grant_pending_ = true;
                to_schedule.push_back(waiter->wake_state_);
            }

            // Only add permits not consumed by woken waiters
            const int remaining = count - to_wake;
            assert(count_ <= INT_MAX - remaining && "semaphore count overflow");
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
    elio::detail::intrusive_list<acquire_awaitable> waiters_;

    detail::wake_state_ptr recover_cancelled_handoff_locked() noexcept {
        if (!waiters_.empty()) {
            auto* waiter = waiters_.pop_front();
            waiter->grant_pending_ = true;
            return waiter->wake_state_;
        }

        assert(count_ < INT_MAX && "semaphore count overflow");
        ++count_;
        return nullptr;
    }
};

} // namespace elio::sync
