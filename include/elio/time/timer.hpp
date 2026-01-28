#pragma once

#include <elio/io/io_awaitables.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/log/macros.hpp>
#include <chrono>
#include <coroutine>
#include <atomic>

namespace elio::time {

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
        
        if (!ctx->prepare(req)) {
            // Failed to prepare, fall back to thread sleep
            ELIO_LOG_WARNING("sleep_awaitable: failed to prepare timeout, using thread sleep");
            std::this_thread::sleep_for(std::chrono::nanoseconds(duration_ns_));
            awaiter.resume();
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
        awaiter_ = awaiter;
        
        // Check if already cancelled before setting up
        if (token_.is_cancelled()) {
            cancelled_ = true;
            awaiter.resume();
            return;
        }
        
        // Get io_context from current worker or use provided one
        io::io_context* ctx = ctx_;
        if (!ctx) {
            ctx = &io::current_io_context();
        }
        ctx_ = ctx;  // Save for later cancel
        
        // Save worker pointer for thread-safe cancellation
        worker_ = runtime::worker_thread::current();
        
        // Create a fire-and-forget cancel executor coroutine
        // This will be scheduled on the worker thread to execute the actual cancel
        cancel_executor_handle_ = create_cancel_executor();
        
        // Register cancellation callback before setting up the timer
        // The callback schedules the cancel executor on the worker thread
        // This ensures io_uring operations are only called from the owning thread
        cancel_registration_ = token_.on_cancel([this]() {
            cancelled_ = true;
            // Schedule cancel execution on the correct worker thread
            // This is thread-safe because schedule() uses lock-free MPSC queue
            if (worker_ && cancel_executor_handle_) {
                // Mark as handed off BEFORE scheduling - this prevents double-free
                // since the coroutine will auto-destroy via final_suspend returning suspend_never
                cancel_executor_handed_off_.store(true, std::memory_order_release);
                worker_->schedule(cancel_executor_handle_);
            }
        });
        
        // Check again after registration (in case cancelled between check and register)
        if (token_.is_cancelled()) {
            cancel_registration_.unregister();
            cancelled_ = true;
            // Destroy unused cancel executor
            if (cancel_executor_handle_) {
                cancel_executor_handle_.destroy();
                cancel_executor_handle_ = nullptr;
            }
            awaiter.resume();
            return;
        }
        
        // Use io_context timeout mechanism
        io::io_request req{};
        req.op = io::io_op::timeout;
        req.length = static_cast<size_t>(duration_ns_);
        req.awaiter = awaiter;
        
        if (!ctx->prepare(req)) {
            cancel_registration_.unregister();
            // Destroy unused cancel executor
            if (cancel_executor_handle_) {
                cancel_executor_handle_.destroy();
                cancel_executor_handle_ = nullptr;
            }
            // Failed to prepare, fall back to polling sleep
            ELIO_LOG_WARNING("cancellable_sleep: failed to prepare timeout, using polling sleep");
            auto end_time = std::chrono::steady_clock::now() + 
                           std::chrono::nanoseconds(duration_ns_);
            while (std::chrono::steady_clock::now() < end_time) {
                if (token_.is_cancelled()) {
                    cancelled_ = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            awaiter.resume();
            return;
        }
        
        ctx->submit();
    }
    
    cancel_result await_resume() noexcept {
        // Unregister callback to prevent use-after-free
        cancel_registration_.unregister();
        // Only destroy cancel executor if it wasn't handed off to worker thread
        // If handed off, it will auto-destroy via final_suspend returning suspend_never
        if (!cancel_executor_handed_off_.load(std::memory_order_acquire) && cancel_executor_handle_) {
            cancel_executor_handle_.destroy();
        }
        cancel_executor_handle_ = nullptr;
        // Check both the flag and the token state (for await_ready() early return case)
        return (cancelled_ || token_.is_cancelled()) ? cancel_result::cancelled 
                                                      : cancel_result::completed;
    }
    
private:
    /// Fire-and-forget coroutine that executes the actual cancel operation
    /// This runs on the worker thread that owns the io_context
    struct cancel_executor {
        struct promise_type {
            cancellable_sleep_awaitable* self = nullptr;
            
            cancel_executor get_return_object() { 
                return {std::coroutine_handle<promise_type>::from_promise(*this)}; 
            }
            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_never final_suspend() noexcept { return {}; }  // Self-destroy
            void return_void() noexcept {}
            void unhandled_exception() noexcept {}
        };
        std::coroutine_handle<promise_type> handle;
    };
    
    std::coroutine_handle<> create_cancel_executor() {
        auto executor = [](cancellable_sleep_awaitable* self) -> cancel_executor {
            // Execute the actual cancel on the worker thread
            if (self->ctx_) {
                self->ctx_->cancel(self->awaiter_.address());
            }
            co_return;
        }(this);
        
        // Store self pointer in promise for access during execution
        executor.handle.promise().self = this;
        return executor.handle;
    }
    
    io::io_context* ctx_ = nullptr;
    runtime::worker_thread* worker_ = nullptr;
    int64_t duration_ns_;
    coro::cancel_token token_;
    coro::cancel_token::registration cancel_registration_;
    std::coroutine_handle<> awaiter_;
    std::coroutine_handle<> cancel_executor_handle_;
    std::atomic<bool> cancel_executor_handed_off_{false};
    bool cancelled_ = false;
};

/// Sleep for a duration
/// @param duration Duration to sleep
/// @return Awaitable that completes after the duration
template<typename Rep, typename Period>
inline auto sleep_for(std::chrono::duration<Rep, Period> duration) {
    return sleep_awaitable(duration);
}

/// Sleep for a duration using a specific io_context
template<typename Rep, typename Period>
inline auto sleep_for(io::io_context& ctx, std::chrono::duration<Rep, Period> duration) {
    return sleep_awaitable(ctx, duration);
}

/// Sleep for a duration with cancellation support
/// @param duration Duration to sleep
/// @param token Cancellation token - sleep returns early if cancelled
/// @return Awaitable that returns cancel_result::completed or cancel_result::cancelled
template<typename Rep, typename Period>
inline auto sleep_for(std::chrono::duration<Rep, Period> duration, coro::cancel_token token) {
    return cancellable_sleep_awaitable(duration, std::move(token));
}

/// Sleep for a duration with cancellation support using a specific io_context
template<typename Rep, typename Period>
inline auto sleep_for(io::io_context& ctx, std::chrono::duration<Rep, Period> duration,
                      coro::cancel_token token) {
    return cancellable_sleep_awaitable(ctx, duration, std::move(token));
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
        auto* sched = runtime::scheduler::current();
        
        if (worker && sched) {
            // Check if task has affinity for a different worker
            size_t affinity = coro::get_affinity(awaiter.address());
            if (affinity != coro::NO_AFFINITY && affinity != worker->worker_id()) {
                // Task has affinity for different worker - use spawn to route correctly
                sched->spawn(awaiter);
                return;
            }
            // No affinity or affinity matches - use fast local path
            worker->schedule_local(awaiter);
            return;
        }
        
        // Slow path: go through scheduler's spawn mechanism
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
