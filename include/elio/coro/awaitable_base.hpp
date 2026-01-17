#pragma once

#include <coroutine>
#include <type_traits>

namespace elio::coro {

/// CRTP base class for custom awaitable types
/// Derived classes must implement:
/// - bool await_ready_impl() const noexcept
/// - void/auto await_suspend_impl(std::coroutine_handle<> awaiter)
/// - T await_resume_impl()
template<typename Derived>
class awaitable_base {
public:
    /// Awaitable interface: Check if already ready
    bool await_ready() const noexcept {
        return static_cast<const Derived*>(this)->await_ready_impl();
    }
    
    /// Awaitable interface: Called when suspending
    /// Returns either void or std::coroutine_handle<> for symmetric transfer
    auto await_suspend(std::coroutine_handle<> awaiter) noexcept {
        return static_cast<Derived*>(this)->await_suspend_impl(awaiter);
    }
    
    /// Awaitable interface: Called when resuming, returns the result
    auto await_resume() {
        return static_cast<Derived*>(this)->await_resume_impl();
    }
};

} // namespace elio::coro
