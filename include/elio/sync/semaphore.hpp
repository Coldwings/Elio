#pragma once

#include <cassert>
#include <coroutine>
#include <atomic>
#include <mutex>
#include <queue>
#include <vector>
#include <algorithm>
#include "../runtime/scheduler.hpp"

namespace elio::sync {

/// Coroutine-aware semaphore
class semaphore {
public:
    explicit semaphore(int initial_count = 0)
        : count_(initial_count) {
        assert(initial_count >= 0 && "semaphore initial count must be non-negative");
    }

    ~semaphore() = default;

    // Non-copyable, non-movable
    semaphore(const semaphore&) = delete;
    semaphore& operator=(const semaphore&) = delete;
    semaphore(semaphore&&) = delete;
    semaphore& operator=(semaphore&&) = delete;

    /// Acquire awaitable
    class acquire_awaitable {
    public:
        explicit acquire_awaitable(semaphore& s) : sem_(s) {}

        bool await_ready() const noexcept {
            return sem_.try_acquire();
        }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(sem_.mutex_);

            if (sem_.count_ > 0) {
                --sem_.count_;
                return false;  // Don't suspend
            }

            sem_.waiters_.push(awaiter);
            return true;  // Suspend
        }

        void await_resume() const noexcept {}

    private:
        semaphore& sem_;
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

        std::vector<std::coroutine_handle<>> to_resume;

        {
            std::lock_guard<std::mutex> guard(mutex_);

            // Calculate how many waiters to wake (up to 'count')
            const int to_wake = std::min(count, static_cast<int>(waiters_.size()));

            // Wake up the calculated number of waiters
            for (int i = 0; i < to_wake; ++i) {
                to_resume.push_back(waiters_.front());
                waiters_.pop();
            }

            // Only add permits not consumed by woken waiters
            const int remaining = count - to_wake;
            assert(count_ <= INT_MAX - remaining && "semaphore count overflow");
            count_ += remaining;
        }

        // Re-schedule waiters through the scheduler
        for (auto& h : to_resume) {
            runtime::schedule_handle(h);
        }
    }

    /// Get current count
    int count() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return count_;
    }

private:
    mutable std::mutex mutex_;
    int count_;
    std::queue<std::coroutine_handle<>> waiters_;
};

} // namespace elio::sync
