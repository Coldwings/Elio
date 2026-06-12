#pragma once

#include <coroutine>
#include <atomic>
#include <mutex>
#include <queue>
#include <vector>
#include <concepts>
#include "../runtime/scheduler.hpp"
#include "mutex.hpp"

namespace elio::sync {

/// Lock concept for condition_variable - any type with lock()/unlock()
/// that can be used with the condition_variable
namespace detail {

template<typename Lock>
concept lockable = requires(Lock& l) {
    l.unlock();
    l.lock();
};

} // namespace detail

/// Coroutine-aware condition variable
/// Suspends the coroutine instead of blocking the thread.
///
/// Can be used with:
/// - elio::sync::mutex (via co_await cv.wait(mutex))
/// - elio::sync::spinlock (via co_await cv.wait(spinlock))
/// - No lock at all (via co_await cv.wait_unlocked()) when all participants
///   are guaranteed to run on the same worker thread
///
/// Supports both notify_one() and notify_all() semantics.
/// The predicate-based wait variants provide spurious-wakeup protection.
class condition_variable {
public:
    condition_variable() = default;
    ~condition_variable() = default;

    // Non-copyable, non-movable
    condition_variable(const condition_variable&) = delete;
    condition_variable& operator=(const condition_variable&) = delete;
    condition_variable(condition_variable&&) = delete;
    condition_variable& operator=(condition_variable&&) = delete;

    /// Internal awaitable: suspends until notified, does NOT re-lock.
    class wait_suspend_awaitable {
    public:
        wait_suspend_awaitable(condition_variable& cv, mutex& m)
            : cv_(cv), mutex_(m) {}

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            {
                std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
                cv_.waiters_.push(awaiter);
            }
            mutex_.unlock();
            return true;
        }

        void await_resume() const noexcept {}

    private:
        condition_variable& cv_;
        mutex& mutex_;
    };

    /// Wait awaitable for use with a generic lockable type (e.g., spinlock)
    /// Atomically releases the lock and suspends the coroutine.
    /// Re-acquires the lock before resuming.
    template<detail::lockable Lock>
    class wait_awaitable_lock {
    public:
        wait_awaitable_lock(condition_variable& cv, Lock& lock)
            : cv_(cv), lock_(lock) {}

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            {
                std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
                cv_.waiters_.push(awaiter);
            }
            // Release the user's lock after enqueuing
            lock_.unlock();
            return true;
        }

        void await_resume() {
            // Re-acquire the lock synchronously (spinlock)
            lock_.lock();
        }

    private:
        condition_variable& cv_;
        Lock& lock_;
    };

    /// Wait awaitable without any external lock
    /// Use only when all participants are guaranteed to run on the same worker thread.
    class wait_awaitable_unlocked {
    public:
        explicit wait_awaitable_unlocked(condition_variable& cv) : cv_(cv) {}

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
            cv_.waiters_.push(awaiter);
            return true;
        }

        void await_resume() const noexcept {}

    private:
        condition_variable& cv_;
    };

    /// Wait with elio::sync::mutex
    /// The mutex must be locked before calling wait().
    /// Atomically releases the mutex, suspends until notified, then re-acquires.
    /// Usage:
    ///   co_await mtx.lock();
    ///   while (!condition) {
    ///       co_await cv.wait(mtx);
    ///   }
    ///   mtx.unlock();
    ///
    /// Note: This returns a task<void> because re-acquiring the mutex requires
    /// an async operation (co_await m.lock()). The template version for generic
    /// lockable types (e.g., spinlock) returns an awaitable directly because
    /// those locks use synchronous lock() calls. Both versions require a single
    /// co_await at the call site.
    coro::task<void> wait(mutex& m) {
        co_await wait_suspend_awaitable(*this, m);
        co_await m.lock();
    }

    /// Wait with a generic lockable (e.g., spinlock)
    /// The lock must be held before calling wait().
    /// Usage:
    ///   sl.lock();
    ///   while (!condition) {
    ///       co_await cv.wait(sl);
    ///   }
    ///   sl.unlock();
    template<detail::lockable Lock>
    auto wait(Lock& lock) {
        return wait_awaitable_lock<Lock>(*this, lock);
    }

    /// Wait without external lock (single-worker only)
    /// Usage:
    ///   while (!condition) {
    ///       co_await cv.wait_unlocked();
    ///   }
    auto wait_unlocked() {
        return wait_awaitable_unlocked(*this);
    }

    /// Wake one waiting coroutine
    void notify_one() {
        std::coroutine_handle<> to_resume;
        {
            std::lock_guard<std::mutex> guard(internal_mutex_);
            if (waiters_.empty()) return;
            to_resume = waiters_.front();
            waiters_.pop();
        }
        runtime::schedule_handle(to_resume);
    }

    /// Wake all waiting coroutines
    void notify_all() {
        std::vector<std::coroutine_handle<>> to_resume;
        {
            std::lock_guard<std::mutex> guard(internal_mutex_);
            while (!waiters_.empty()) {
                to_resume.push_back(waiters_.front());
                waiters_.pop();
            }
        }
        for (auto& h : to_resume) {
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
    std::queue<std::coroutine_handle<>> waiters_;
};

} // namespace elio::sync
