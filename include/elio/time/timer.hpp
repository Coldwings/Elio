#pragma once

#include <elio/io/io_context.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/log/macros.hpp>
#include <chrono>
#include <coroutine>

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
        // Get io_context from scheduler or use provided one
        io::io_context* ctx = ctx_;
        if (!ctx) {
            auto* sched = runtime::scheduler::current();
            if (sched) {
                ctx = sched->get_io_context();
            }
        }
        
        if (!ctx) {
            // No io_context available, fall back to thread sleep
            ELIO_LOG_DEBUG("sleep_awaitable: no io_context, using thread sleep");
            std::this_thread::sleep_for(std::chrono::nanoseconds(duration_ns_));
            awaiter.resume();
            return;
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
        // Reschedule on the current scheduler
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
