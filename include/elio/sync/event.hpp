#pragma once

#include <coroutine>
#include <atomic>
#include <mutex>
#include <queue>
#include <vector>
#include "../runtime/scheduler.hpp"

namespace elio::sync {

/// Coroutine-aware event (manual reset)
class event {
public:
    event() = default;
    ~event() = default;

    // Non-copyable, non-movable
    event(const event&) = delete;
    event& operator=(const event&) = delete;
    event(event&&) = delete;
    event& operator=(event&&) = delete;

    /// Wait awaitable
    class wait_awaitable {
    public:
        explicit wait_awaitable(event& e) : evt_(e) {}

        bool await_ready() const noexcept {
            return evt_.signaled_.load(std::memory_order_acquire);
        }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(evt_.mutex_);

            if (evt_.signaled_.load(std::memory_order_relaxed)) {
                return false;  // Already signaled
            }

            evt_.waiters_.push(awaiter);
            return true;
        }

        void await_resume() const noexcept {}

    private:
        event& evt_;
    };

    /// Wait for the event to be signaled
    auto wait() {
        return wait_awaitable(*this);
    }

    /// Signal the event (wake all waiters)
    void set() {
        std::vector<std::coroutine_handle<>> to_resume;

        {
            std::lock_guard<std::mutex> guard(mutex_);
            signaled_.store(true, std::memory_order_release);

            while (!waiters_.empty()) {
                to_resume.push_back(waiters_.front());
                waiters_.pop();
            }
        }

        // Re-schedule waiters through the scheduler
        for (auto& h : to_resume) {
            runtime::schedule_handle(h);
        }
    }

    /// Reset the event
    void reset() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        signaled_.store(false, std::memory_order_release);
    }

    /// Check if signaled
    bool is_set() const noexcept {
        return signaled_.load(std::memory_order_acquire);
    }

private:
    std::mutex mutex_;
    std::atomic<bool> signaled_{false};
    std::queue<std::coroutine_handle<>> waiters_;
};

} // namespace elio::sync
