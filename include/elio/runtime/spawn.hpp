#pragma once

#include <functional>
#include <type_traits>
#include <utility>
#include "../coro/task.hpp"
#include "../coro/vthread_stack.hpp"
#include "scheduler.hpp"

namespace elio {

namespace detail {
    // Type traits for task<T>
    template<typename T> struct task_value;
    template<typename T> struct task_value<coro::task<T>> { using type = T; };
    template<typename T> using task_value_t = typename task_value<T>::type;

    template<typename T> struct is_task : std::false_type {};
    template<typename T> struct is_task<coro::task<T>> : std::true_type {};
    template<typename T> inline constexpr bool is_task_v = is_task<T>::value;
} // namespace detail

/// Fire-and-forget: spawn a coroutine without awaiting its result.
/// The coroutine runs independently and self-destructs on completion.
///
/// @tparam F  Callable type that returns a task<T>
/// @tparam Args  Argument types
/// @param f  Callable to invoke (must return a task)
/// @param args  Arguments to forward to the callable
///
/// Example:
///   elio::go(async_work);
///   elio::go(async_work_with_args, 1, 2, 3);
///   elio::go([&]() -> coro::task<void> { co_await some_async_op(); });
template<typename F, typename... Args>
    requires (std::invocable<F, Args...> && detail::is_task_v<std::invoke_result_t<F, Args...>>)
void go(F&& f, Args&&... args) {
    coro::detail::heap_alloc_guard guard;
    auto t = std::invoke(std::forward<F>(f), std::forward<Args>(args)...);

    auto handle = coro::detail::task_access::release(t);
    handle.promise().detached_ = true;
    auto* vstack = new coro::vthread_stack();
    handle.promise().set_vstack_owner(vstack);
    // Detach from current thread's frame chain before spawning to another thread
    // to avoid use-after-free when this thread creates another coroutine.
    handle.promise().detach_from_parent();
    runtime::schedule_handle(handle);
}

/// Spawn a coroutine and return a join_handle to await its result.
/// The coroutine runs concurrently and the result can be retrieved via co_await.
///
/// @tparam F  Callable type that returns a task<T>
/// @tparam Args  Argument types
/// @param f  Callable to invoke (must return a task)
/// @param args  Arguments to forward to the callable
/// @return join_handle<T> that can be awaited to get the result
///
/// Example:
///   auto handle = elio::spawn(compute_async, input);
///   auto result = co_await handle;
template<typename F, typename... Args>
    requires (std::invocable<F, Args...> && detail::is_task_v<std::invoke_result_t<F, Args...>>)
auto spawn(F&& f, Args&&... args)
    -> coro::join_handle<detail::task_value_t<std::invoke_result_t<F, Args...>>>
{
    using T = detail::task_value_t<std::invoke_result_t<F, Args...>>;
    coro::detail::heap_alloc_guard guard;
    auto t = std::invoke(std::forward<F>(f), std::forward<Args>(args)...);

    auto handle = coro::detail::task_access::release(t);
    auto state = std::make_shared<coro::detail::join_state<T>>();
    handle.promise().join_state_ = state;
    auto* vstack = new coro::vthread_stack();
    handle.promise().set_vstack_owner(vstack);
    // Detach from current thread's frame chain before spawning to another thread
    // to avoid use-after-free when this thread creates another coroutine.
    handle.promise().detach_from_parent();
    runtime::schedule_handle(handle);
    return coro::join_handle<T>(std::move(state));
}

} // namespace elio

// Macros — syntactic sugar for inline lambda coroutines
// These capture by reference and wrap the expression in a lambda returning task

/// Fire-and-forget macro for inline coroutine expressions
/// Usage: ELIO_GO(some_async_operation())
#define ELIO_GO(...)    elio::go([&]() { return __VA_ARGS__; })

/// Spawn macro for inline coroutine expressions, returns join_handle
/// Usage: auto h = ELIO_SPAWN(compute_async()); auto result = co_await h;
#define ELIO_SPAWN(...) elio::spawn([&]() { return __VA_ARGS__; })
