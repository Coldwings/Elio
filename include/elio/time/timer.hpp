#pragma once

#include <elio/io/io_awaitables.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/log/macros.hpp>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

#ifdef __linux__
#include <linux/time_types.h>  // For __kernel_timespec (available on all Linux)
#endif

namespace elio::time {

namespace detail {

/// Submit `work` to the scheduler's blocking pool, falling back to a
/// detached std::thread when there is no scheduler (e.g. unit tests).
/// Used by the timer SQ-full fallback paths so we never block a worker
/// thread for the full sleep duration.
///
/// During scheduler shutdown the work is silently dropped. Callers pass a
/// lambda that captures a raw ``runtime::scheduler*`` and the awaiter's
/// ``coroutine_handle``; running that on a detached thread once shutdown
/// is in progress is unsafe two ways:
///
///   * the scheduler may be destroyed between the submit and the resume
///     so ``sched->is_running()`` / ``sched->spawn(awaiter)`` reads a
///     dangling pointer;
///   * even while the scheduler is alive but ``is_running() == false``,
///     ``resume_via_scheduler`` falls through to ``handle.resume()`` on
///     the detached thread — which has no ``worker_thread::current()``,
///     no ``current_io_context()`` and no ``vthread_stack``, so every
///     subsequent ``co_await`` on the resumed coroutine is a UAF.
///
/// Returns true if the work was accepted (by the blocking pool or a
/// detached thread). Returns false if the scheduler is shutting down
/// and the work was dropped — the caller MUST resume or destroy the
/// awaiter to avoid leaking the coroutine frame.
template <typename F>
inline bool submit_blocking(F&& work) {
    auto* sched = runtime::get_current_scheduler();
    if (sched) {
        auto* pool = sched->get_blocking_pool();
        if (pool && sched->is_running()) {
            std::function<void()> task{std::forward<F>(work)};
            if (pool->submit(std::move(task))) {
                return true;
            }
            // Pool rejection => scheduler is tearing down. Drop the work;
            // see the doc comment above for why a detached-thread fallback
            // here would UAF the awaiter / scheduler.
            return false;
        }
        if (!sched->is_running()) {
            // Scheduler already shut down: same UAF concerns as the pool
            // rejection branch. Drop.
            return false;
        }
        // Scheduler exists, is running, but exposes no pool: fall through
        // to the standalone detached-thread path.
    }
    // Standalone use (no scheduler) or scheduler-running-without-pool:
    // detached thread is safe because the work captures either a null
    // ``sched`` (resume falls through to direct ``handle.resume()`` with
    // no scheduler context to dangle) or a scheduler that stays alive
    // past the resume.
    std::thread(std::forward<F>(work)).detach();
    return true;
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
/// Uses the I/O backend's timeout mechanism for efficient waiting.
///
/// Inherits from ``io::io_awaitable_base`` so the SQE carries a tagged
/// ``op_state`` pointer rather than the raw coroutine handle as user_data.
/// If the awaitable's frame is destroyed before the timeout CQE arrives
/// (e.g. parent task forced down on scheduler shutdown), the base
/// destructor CASes the op_state from ``pending`` to ``orphaned`` and the
/// completion handler drops the late CQE instead of resuming a dangling
/// handle. See the contract in ``include/elio/io/io_backend.hpp``.
class sleep_awaitable : public io::io_awaitable_base {
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
        // Owner-controlled op_state: tags the SQE so a CQE arriving after
        // teardown is dropped instead of resuming a freed coroutine frame.
        req.state = setup_op_state(awaiter);
#ifdef __linux__
        // Provide our local timespec for io_uring backend (runtime check)
        // epoll backend ignores this field
        req.timeout_ts = &ts_;
#endif

        if (!ctx->prepare(req)) {
            // SQ full / submit failed: no SQE went out, so no CQE will
            // arrive. Drop the op_state (no orphan needed) and route the
            // sleep to the blocking pool so we don't block this worker
            // thread for the full duration — that would halt every other
            // coroutine on this worker and violate the "coroutines must
            // not block worker threads" invariant.
            clear_op_state();
            ELIO_LOG_WARNING("sleep_awaitable: failed to prepare timeout, routing fallback through blocking pool");
            auto* sched = runtime::get_current_scheduler();
            const int64_t duration = duration_ns_;
            if (!detail::submit_blocking([awaiter, sched, duration]() {
                std::this_thread::sleep_for(std::chrono::nanoseconds(duration));
                detail::resume_via_scheduler(awaiter, sched);
            })) {
                // Scheduler shutting down: work was dropped. Destroy the
                // awaiter handle to prevent coroutine frame leak.
                if (awaiter && !awaiter.done()) {
                    awaiter.destroy();
                }
            }
            return;
        }

        ctx->submit();
    }

    void await_resume() noexcept {
        // Nothing to return; op_state lifetime is handled by the base
        // destructor (deletes via unique_ptr in the completed phase).
    }

private:
    io::io_context* ctx_ = nullptr;
    int64_t duration_ns_;
#ifdef __linux__
    mutable __kernel_timespec ts_{};  // Storage for io_uring timeout (always available on Linux)
#endif
};

/// Awaitable for cancellable sleep operations
/// Returns cancel_result indicating if sleep completed or was cancelled.
///
/// Inherits from ``io::io_awaitable_base`` for the same UAF protection as
/// ``sleep_awaitable`` — a CQE arriving after teardown finds an orphaned
/// op_state and is dropped. The cancel callback path additionally uses
/// the tagged op_state pointer as the cancel SQE's user_data so it
/// matches the original timer SQE.
class cancellable_sleep_awaitable : public io::io_awaitable_base {
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

    bool await_suspend(std::coroutine_handle<> awaiter) {
        // Check if already cancelled before setting up
        if (token_.is_cancelled()) {
            already_cancelled_before_setup_ = true;
            return false;  // Resume immediately (do not suspend)
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
        // ``op`` is set below once setup_op_state succeeds. The cancel
        // executor uses it as the SQE-matching key (tagged user_data) so
        // it cancels the right entry even though the SQE no longer keys
        // off the raw coroutine handle.
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
            // owns the ring. We set affinity to prevent stealing — this
            // task accesses worker-local io_context and must run on the
            // correct worker.
            auto exec = make_cancel_executor(state);
            if (auto* promise = coro::get_promise_base(exec.handle.address())) {
                promise->set_affinity(state->worker->worker_id());
                promise->set_worker_local();
                promise->detach_from_parent();
            }
            state->worker->schedule_or_destroy(exec.handle);
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
            return false;  // Resume immediately (do not suspend)
        }

        // Use io_context timeout mechanism
        io::io_request req{};
        req.op = io::io_op::timeout;
        req.length = static_cast<size_t>(duration_ns_);
        req.awaiter = awaiter;
        // Owner-controlled op_state: same UAF protection as
        // sleep_awaitable. The pointer is stashed in shared_state so the
        // cancel executor can pass the matching tagged user_data to
        // io_context::cancel().
        req.state = setup_op_state(awaiter);
        state->op = req.state;
#ifdef __linux__
        // Provide our local timespec for io_uring backend (runtime check)
        // epoll backend ignores this field
        req.timeout_ts = &ts_;
#endif

        if (ctx->prepare(req)) {
            ctx->submit();
            return true;  // Suspend (wait for timeout to complete)
        }

        // SQ full / submit failed: no SQE went out, drop the op_state
        // (no CQE will arrive) and route through the blocking pool.
        // We also drop the cancel registration — blocking the worker for
        // the full duration would freeze every other coroutine on it.
        clear_op_state();
        state->op = nullptr;
        cancel_registration_.unregister();
        ELIO_LOG_WARNING("cancellable_sleep: failed to prepare timeout, routing fallback through blocking pool");

        auto* sched = runtime::get_current_scheduler();
        const int64_t duration = duration_ns_;
        coro::cancel_token token_copy = token_;

        if (!detail::submit_blocking(
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
            })) {
            // Scheduler shutting down: work was dropped. Destroy the
            // awaiter handle to prevent coroutine frame leak. The
            // coroutine was already popped from the worker deque and
            // suspended via await_suspend returning void, so
            // drain_remaining_tasks will NOT find it.
            if (awaiter && !awaiter.done()) {
                awaiter.destroy();
            }
        }
        return true;  // Suspend (blocking pool will resume when done)
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
            // Set resumed = true to signal to any cancel executor that
            // has not yet run that the awaiter is already done. Safety
            // is structural: both backends call safe_resume() (i.e.
            // handle.resume()) synchronously inside poll(), so
            // await_resume() always executes before the worker returns
            // to get_next_task() and drains the inbox where the cancel
            // executor waits. The store here thus always precedes the
            // cancel executor's load in program order on the worker
            // thread.
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
    ///
    /// ``op`` is the awaitable's owned op_state pointer. The cancel
    /// executor uses ``tagged_op_state_user_data(op)`` as the SQE key
    /// when issuing the cancel — that matches what the timer SQE was
    /// submitted with. The pointer is only ever used as a key, never
    /// dereferenced through the cancel path, so its eventual deletion by
    /// the backend (after orphan + CQE) is benign.
    struct shared_state {
        io::io_context* ctx = nullptr;
        std::coroutine_handle<> awaiter;
        runtime::worker_thread* worker = nullptr;
        io::op_state* op = nullptr;
        std::atomic<bool> resumed{false};
        std::atomic<bool> cancelled{false};
    };

    /// Fire-and-forget coroutine that executes io_context::cancel() on
    /// the worker that owns the ring. Self-destroys via suspend_never on
    /// final_suspend. Captures shared_ptr<shared_state> by value so it
    /// holds an independent ref to the state.
    ///
    /// NOTE: promise_type MUST inherit coro::promise_base to enable the
    /// affinity mechanism. Without it, try_steal() sees NO_AFFINITY and
    /// allows stealing this task to other workers, which then access the
    /// wrong io_context's io_uring ring — causing data races (TSAN warning
    /// in CI run 27592314235).
    struct cancel_executor {
        struct promise_type : public coro::promise_base {
            cancel_executor get_return_object() {
                return {std::coroutine_handle<promise_type>::from_promise(*this)};
            }
            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_never final_suspend() noexcept { return {}; }
            void return_void() noexcept {}
        };
        std::coroutine_handle<promise_type> handle;
    };

    static cancel_executor make_cancel_executor(std::shared_ptr<shared_state> state) {
        // Guard against running when the timer already completed naturally.
        // Safety invariant: both io_uring and epoll backends call
        // safe_resume() (handle.resume()) synchronously inside poll(),
        // so await_resume() — which stores resumed=true — is always
        // executed before the worker returns to get_next_task() and
        // drains the inbox queue where this coroutine waits. Concretely:
        //   * Timer CQE arrives inside poll_io_when_idle()
        //   * resume_deferred() calls safe_resume(timer_coroutine) inline
        //   * await_resume() stores resumed=true, returns
        //   * poll() returns, poll_io_when_idle() returns
        //   * Main loop: get_next_task() drains inbox → this coroutine
        //     is dequeued and sees resumed==true → co_return safely
        //
        // For the normal early-cancel path (cancel fires before the
        // timer CQE), resumed is still false here and we proceed to
        // issue ctx->cancel() as intended.
        if (state->resumed.load(std::memory_order_acquire)) {
            co_return;
        }
        if (state->ctx && state->op) {
            // Match the tagged user_data the timer SQE was submitted
            // with. ``state->op`` is an opaque key here — never
            // dereferenced — so it stays sound even if the backend has
            // already freed the op_state via the orphan/CQE handshake.
            state->ctx->cancel(io::tagged_op_state_user_data(state->op));
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
