#pragma once

#include <coroutine>
#include <atomic>
#include <mutex>
#include <vector>
#include <concepts>
#include <cassert>
#include "../detail/intrusive_list.hpp"
#include "../runtime/scheduler.hpp"
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
class condition_variable {
public:
    /// Common base for all cv waiter types.
    /// Inherits intrusive_list_node so all awaiter types can share one list.
    class cv_waiter_base : public elio::detail::intrusive_list_node<cv_waiter_base> {
    public:
        std::coroutine_handle<> handle_;
    protected:
        cv_waiter_base() = default;
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
            // ALWAYS acquire internal_mutex_ to prevent race with notify
            std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
            if (this->is_linked()) {
                cv_.waiters_.remove(this);
            }
        }

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
            // Unlock user mutex BEFORE enqueueing to ensure atomicity
            mutex_.unlock();
            this->handle_ = awaiter;
            cv_.waiters_.push_back(this);
            return true;
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
            std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
            if (this->is_linked()) {
                cv_.waiters_.remove(this);
            }
        }

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
            lock_.unlock();
            this->handle_ = awaiter;
            cv_.waiters_.push_back(this);
            return true;
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
            std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
            if (this->is_linked()) {
                cv_.waiters_.remove(this);
            }
        }

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
            this->handle_ = awaiter;
            cv_.waiters_.push_back(this);
            return true;
        }

        void await_resume() const noexcept {}

    private:
        condition_variable& cv_;
    };

    /// Wait with elio::sync::mutex
    coro::task<void> wait(mutex& m) {
        co_await wait_suspend_awaitable(*this, m);
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
        std::coroutine_handle<> to_schedule = nullptr;
        {
            std::lock_guard<std::mutex> guard(internal_mutex_);
            if (waiters_.empty()) return;

            auto* waiter = waiters_.pop_front();
            to_schedule = waiter->handle_;
        }
        // Schedule outside lock to avoid deadlock if schedule_handle()
        // resumes inline (trampoline path) and destructor re-acquires mutex.
        runtime::schedule_handle(to_schedule);
    }

    /// Wake all waiting coroutines
    void notify_all() {
        std::vector<std::coroutine_handle<>> to_schedule;
        {
            std::lock_guard<std::mutex> guard(internal_mutex_);
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
