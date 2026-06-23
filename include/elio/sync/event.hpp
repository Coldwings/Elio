#pragma once

#include <coroutine>
#include <atomic>
#include <mutex>
#include <vector>
#include <cassert>
#include "../detail/intrusive_list.hpp"
#include "../runtime/scheduler.hpp"

namespace elio::sync {

/// Coroutine-aware event (manual reset)
class event {
public:
    // Forward declaration for intrusive_list
    class wait_awaitable;

    event() = default;

    ~event() {
        assert(waiters_.empty() && "event destroyed with pending waiters");
    }

    // Non-copyable, non-movable
    event(const event&) = delete;
    event& operator=(const event&) = delete;
    event(event&&) = delete;
    event& operator=(event&&) = delete;

    /// Wait awaitable — inherits intrusive_list_node for safe unlinking
    class wait_awaitable : public detail::intrusive_list_node<wait_awaitable> {
    public:
        explicit wait_awaitable(event& e) : evt_(e) {}

        ~wait_awaitable() {
            // ALWAYS acquire mutex to prevent race with set().
            // If set() already popped us and is scheduling, holding the
            // mutex ensures the coroutine frame won't be destroyed until
            // schedule_handle() completes.
            std::lock_guard<std::mutex> guard(evt_.mutex_);
            if (this->is_linked()) {
                evt_.waiters_.remove(this);
            }
        }

        bool await_ready() const noexcept {
            return evt_.signaled_.load(std::memory_order_acquire);
        }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(evt_.mutex_);

            if (evt_.signaled_.load(std::memory_order_relaxed)) {
                return false;  // Already signaled
            }

            handle_ = awaiter;
            evt_.waiters_.push_back(this);
            return true;
        }

        void await_resume() const noexcept {}

    private:
        event& evt_;
        std::coroutine_handle<> handle_;

        friend class event;
    };

    /// Wait for the event to be signaled
    auto wait() {
        return wait_awaitable(*this);
    }

    /// Signal the event (wake all waiters)
    void set() {
        std::vector<std::coroutine_handle<>> to_schedule;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            signaled_.store(true, std::memory_order_release);

            // Collect handles and pop from list under lock.
            // Popping marks nodes as unlinked, so destructors won't try to remove them.
            while (!waiters_.empty()) {
                auto* waiter = waiters_.pop_front();
                to_schedule.push_back(waiter->handle_);
            }
        }
        // Schedule outside lock to avoid deadlock if schedule_handle()
        // resumes inline (trampoline path) and destructor re-acquires mutex.
        for (auto h : to_schedule) {
            runtime::schedule_handle(h);
        }
    }

    /// Reset the event
    void reset() {
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
    detail::intrusive_list<wait_awaitable> waiters_;
};

} // namespace elio::sync
