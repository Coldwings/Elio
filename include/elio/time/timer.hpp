#pragma once

#include <elio/io/io_awaitables.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/log/macros.hpp>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <memory>
#include <thread>
#include <utility>

#ifdef __linux__
#include <linux/time_types.h>  // For __kernel_timespec (available on all Linux)
#endif

namespace elio::time {

namespace detail {

/// Submit `work` to the scheduler's blocking pool, falling back to a
/// detached std::thread if no pool is available. Used by the timer
/// SQ-full fallback paths so we never block a worker thread for the
/// full sleep duration.
template <typename F>
inline void submit_blocking(F&& work) {
    auto* sched = runtime::get_current_scheduler();
    if (sched && sched->is_running()) {
        if (auto* pool = sched->get_blocking_pool()) {
            pool->submit(std::forward<F>(work));
            return;
        }
    }
    std::thread(std::forward<F>(work)).detach();
}

/// Resume a coroutine handle on the right scheduler context. Used from
/// blocking-pool worker threads where direct `handle.resume()` would run
/// the resumed coroutine on the wrong thread.
inline void resume_via_scheduler(std::coroutine_handle<> handle,
                                 runtime::scheduler* sched) noexcept {
    if (!handle) return;
    if (sched && sched->is_running()) {
        sched->spawn(handle);
        return;
    }
    if (!handle.done()) {
        handle.resume();
    }
}

}  // namespace detail

/// Awaitable for sleeping/delaying execution
/// Uses the I/O backend's timeout mechanism for efficient waiting
class sleep_awaitable {
public:
    /// Construct a sleep awaitable
    /// @param duration Duration to sleep
    template<typename Rep, typename Period>
    explicit sleep_awaitable(std::chrono::duration<Rep, Period> duration)
        : duration_ns_(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count()) {}

    /// Construct with explicit io_context
    template<typename Rep, typename Period>
    sleep_awaitable(io::io_context& ctx, std::chrono::duration<Rep, Period> duration)
        : ctx_(&ctx)
        , duration_ns_(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count()) {}

    bool await_ready() const noexcept {
        // If duration is zero or negative, complete immediately
        return duration_ns_ <= 0;
    }

    void await_suspend(std::coroutine_handle<> awaiter) {
        // Get io_context from current worker or use provided one
        io::io_context* ctx = ctx_;
        if (!ctx) {
            ctx = &io::current_io_context();
        }

        // Use io_context timeout mechanism
        io::io_request req{};
        req.op = io::io_op::timeout;
        req.length = static_cast<size_t>(duration_ns_);
        req.awaiter = awaiter;
#ifdef __linux__
        // Provide our local timespec for io_uring backend (runtime check)
        // epoll backend ignores this field
        req.timeout_ts = &ts_;
#endif

        if (!ctx->prepare(req)) {
            // SQ full / submit failed. Route the sleep to the blocking pool
            // so we don't block this worker thread for the full duration —
            // that would halt every other coroutine on this worker and
            // violate the "coroutines must not block worker threads"
            // invariant.
            ELIO_LOG_WARNING("sleep_awaitable: failed to prepare timeout, routing fallback through blocking pool");
            auto* sched = runtime::get_current_scheduler();
            const int64_t duration = duration_ns_;
            detail::submit_blocking([awaiter, sched, duration]() {
                std::this_thread::sleep_for(std::chrono::nanoseconds(duration));
                detail::resume_via_scheduler(awaiter, sched);
            });
            return;
        }

        ctx->submit();
    }

    void await_resume() const noexcept {
        // Nothing to return
    }

private:
    io::io_context* ctx_ = nullptr;
    int64_t duration_ns_;
#ifdef __linux__
    mutable __kernel_timespec ts_{};  // Storage for io_uring timeout (always available on Linux)
#endif
};

/// Awaitable for cancellable sleep operations
/// Returns cancel_result indicating if sleep completed or was cancelled
class cancellable_sleep_awaitable {
public:
    using cancel_result = coro::cancel_result;

    /// Construct with duration and cancel token
    template<typename Rep, typename Period>
    cancellable_sleep_awaitable(std::chrono::duration<Rep, Period> duration,
                                 coro::cancel_token token)
        : duration_ns_(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count())
        , token_(std::move(token)) {}

    /// Construct with io_context, duration, and cancel token
    template<typename Rep, typename Period>
    cancellable_sleep_awaitable(io::io_context& ctx,
                                 std::chrono::duration<Rep, Period> duration,
                                 coro::cancel_token token)
        : ctx_(&ctx)
        , duration_ns_(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count())
        , token_(std::move(token)) {}

    bool await_ready() const noexcept {
        // Complete immediately if already cancelled or duration <= 0
        return token_.is_cancelled() || duration_ns_ <= 0;
    }

    void await_suspend(std::coroutine_handle<> awaiter) {
        // Check if already cancelled before setting up
        if (token_.is_cancelled()) {
            already_cancelled_before_setup_ = true;
            awaiter.resume();
            return;
        }

        // Get io_context from current worker or use provided one
        io::io_context* ctx = ctx_;
        if (!ctx) {
            ctx = &io::current_io_context();
        }
        ctx_ = ctx;

        // Allocate the shared cancel state. The cancel callback and the
        // fire-and-forget cancel-executor coroutine each take their own
        // shared_ptr ref so they can outlive `*this` safely. The previous
        // implementation captured `this` in the cancel executor; once
        // PR #71 made workers park in `poll(-1)`, the natural timer CQE
        // and the cancel inbox entry could be processed in the same poll
        // cycle, leaving the executor running after the awaitable's
        // destructor and producing a UAF on `self->ctx_` / `self->awaiter_`.
        auto state = std::make_shared<shared_state>();
        state->ctx = ctx;
        state->awaiter = awaiter;
        state->worker = runtime::worker_thread::current();
        state_ = state;

        // Register cancellation callback. The lambda captures only the
        // shared_ptr — never `this` — so it stays valid even if the
        // awaitable is destroyed mid-flight on another thread.
        cancel_registration_ = token_.on_cancel([state]() {
            state->cancelled.store(true, std::memory_order_release);
            if (!state->worker) {
                return;
            }
            // Schedule the actual io_context::cancel() on the worker that
            // owns the ring. schedule() uses a lock-free MPSC inbox so this
            // is safe from any thread.
            auto exec = make_cancel_executor(state);
            state->worker->schedule(exec.handle);
        });

        // Check again after registration (cancelled between is_cancelled()
        // above and on_cancel()). If add_callback already saw `cancelled`
        // it invoked the lambda synchronously and returned id 0 — the
        // unregister() below is then a no-op, which is fine.
        if (token_.is_cancelled()) {
            cancel_registration_.unregister();
            // Mark resumed so any executor already queued on the worker
            // observes this state and bails before touching ctx with a
            // stale awaiter address.
            state->resumed.store(true, std::memory_order_release);
            awaiter.resume();
            return;
        }

        // Use io_context timeout mechanism
        io::io_request req{};
        req.op = io::io_op::timeout;
        req.length = static_cast<size_t>(duration_ns_);
        req.awaiter = awaiter;
#ifdef __linux__
        // Provide our local timespec for io_uring backend (runtime check)
        // epoll backend ignores this field
        req.timeout_ts = &ts_;
#endif

        if (ctx->prepare(req)) {
            ctx->submit();
            return;
        }

        // SQ full / submit failed. Drop the cancel registration and route
        // the sleep through the blocking pool — blocking the worker for
        // the full duration would freeze every other coroutine on it.
        cancel_registration_.unregister();
        ELIO_LOG_WARNING("cancellable_sleep: failed to prepare timeout, routing fallback through blocking pool");

        auto* sched = runtime::get_current_scheduler();
        const int64_t duration = duration_ns_;
        coro::cancel_token token_copy = token_;

        detail::submit_blocking(
            [awaiter, sched, duration, token = std::move(token_copy), state]() mutable {
                const auto end_time = std::chrono::steady_clock::now() +
                                      std::chrono::nanoseconds(duration);
                while (std::chrono::steady_clock::now() < end_time) {
                    if (token.is_cancelled()) {
                        state->cancelled.store(true, std::memory_order_release);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                // Mark resumed so any concurrent cancel executor becomes a
                // no-op before touching ctx / awaiter.
                state->resumed.store(true, std::memory_order_release);
                detail::resume_via_scheduler(awaiter, sched);
            });
    }

    cancel_result await_resume() noexcept {
        // Unregister to prevent the cancel callback from firing after
        // teardown. trigger() may still race us; if it has already moved
        // the head into a local list, this becomes a no-op and the
        // callback runs on the cancelling thread — but the lambda only
        // touches the shared state, never `this`.
        cancel_registration_.unregister();
        bool was_cancelled = already_cancelled_before_setup_;
        if (state_) {
            // Set resumed first so any cancel executor that the worker
            // pulls from its inbox after this point sees `resumed == true`
            // and bails before dereferencing `state->ctx` with a stale
            // awaiter address.
            state_->resumed.store(true, std::memory_order_release);
            was_cancelled = was_cancelled ||
                            state_->cancelled.load(std::memory_order_acquire);
        }
        return (was_cancelled || token_.is_cancelled()) ? cancel_result::cancelled
                                                         : cancel_result::completed;
    }

private:
    /// Shared state owned jointly by the awaitable, the cancel callback
    /// lambda, and the cancel executor coroutine. shared_ptr keeps it
    /// alive past the awaitable's destruction so cross-thread cancel
    /// paths cannot UAF.
    struct shared_state {
        io::io_context* ctx = nullptr;
        std::coroutine_handle<> awaiter;
        runtime::worker_thread* worker = nullptr;
        std::atomic<bool> resumed{false};
        std::atomic<bool> cancelled{false};
    };

    /// Fire-and-forget coroutine that executes io_context::cancel() on
    /// the worker that owns the ring. Self-destroys via suspend_never on
    /// final_suspend. Captures shared_ptr<shared_state> by value so it
    /// holds an independent ref to the state.
    struct cancel_executor {
        struct promise_type {
            cancel_executor get_return_object() {
                return {std::coroutine_handle<promise_type>::from_promise(*this)};
            }
            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_never final_suspend() noexcept { return {}; }
            void return_void() noexcept {}
            void unhandled_exception() noexcept {}
        };
        std::coroutine_handle<promise_type> handle;
    };

    static cancel_executor make_cancel_executor(std::shared_ptr<shared_state> state) {
        // Atomically claim the resume slot. If `resumed` was already true,
        // the natural completion path has resumed (or is resuming) the
        // awaiter; touching state->ctx with state->awaiter.address() is
        // still memory-safe (the addresses are just opaque keys to the
        // ring) but we'd risk cancelling a freshly-reused SQE keyed off
        // the same handle address. Bail early.
        if (state->resumed.exchange(true, std::memory_order_acq_rel)) {
            co_return;
        }
        if (state->ctx) {
            state->ctx->cancel(state->awaiter.address());
        }
        co_return;
    }

    io::io_context* ctx_ = nullptr;
    int64_t duration_ns_;
    coro::cancel_token token_;
    coro::cancel_token::registration cancel_registration_;
    std::shared_ptr<shared_state> state_;
    bool already_cancelled_before_setup_ = false;
#ifdef __linux__
    mutable __kernel_timespec ts_{};  // Storage for io_uring timeout (always available on Linux)
#endif
};

/// Sleep for a duration
/// @param duration Duration to sleep
/// @return Awaitable that completes after the duration
template<typename Rep, typename Period>
inline auto sleep_for(std::chrono::duration<Rep, Period> duration) {
    return sleep_awaitable(duration);
}

/// Sleep for a duration with cancellation support
/// @param duration Duration to sleep
/// @param token Cancellation token - sleep returns early if cancelled
/// @return Awaitable that returns cancel_result::completed or cancel_result::cancelled
template<typename Rep, typename Period>
inline auto sleep_for(std::chrono::duration<Rep, Period> duration, coro::cancel_token token) {
    return cancellable_sleep_awaitable(duration, std::move(token));
}

/// Sleep until a time point
/// @param time_point Time point to sleep until
/// @return Awaitable that completes at the time point
template<typename Clock, typename Duration>
inline auto sleep_until(std::chrono::time_point<Clock, Duration> time_point) {
    auto now = Clock::now();
    if (time_point <= now) {
        return sleep_awaitable(std::chrono::nanoseconds(0));
    }
    return sleep_awaitable(time_point - now);
}

/// Yield execution to other coroutines
/// This is a zero-duration sleep that reschedules the coroutine
class yield_awaitable {
public:
    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> awaiter) const {
        auto* worker = runtime::worker_thread::current();

        if (worker) [[likely]] {
            // Fast path: directly schedule to local queue
            // Skip affinity check for performance - yield keeps running on same worker
            // If task has affinity for different worker, it will be routed on next spawn
            worker->schedule_local(awaiter);
            return;
        }

        // Slow path: no current worker, go through scheduler
        auto* sched = runtime::scheduler::current();
        if (sched) {
            sched->spawn(awaiter);
        } else {
            // No scheduler, just resume immediately
            awaiter.resume();
        }
    }

    void await_resume() const noexcept {}
};

/// Yield execution to other coroutines
inline auto yield() {
    return yield_awaitable{};
}

} // namespace elio::time
