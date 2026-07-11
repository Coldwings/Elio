#pragma once

#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include "../coro/traits.hpp"
#include "../coro/vthread_stack.hpp"
#include "../log/macros.hpp"
#include "scheduler.hpp"

namespace elio {

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
    auto* sched = runtime::scheduler::current();
    if (sched && sched->is_running()) {
        sched->go(std::forward<F>(f), std::forward<Args>(args)...);
        return;
    }
    ELIO_LOG_ERROR("elio::go() called without a running scheduler — aborting");
    std::abort();
}

/// Fire-and-forget: spawn a coroutine pinned to a specific worker.
/// The task is bound to the given worker and cannot be stolen by others.
///
/// @tparam F  Callable type that returns a task<T>
/// @tparam Args  Argument types
/// @param worker_id  Target worker index
/// @param f  Callable to invoke (must return a task)
/// @param args  Arguments to forward to the callable
///
/// Example:
///   elio::go_to(0, async_work);
///   elio::go_to(1, async_work_with_args, 1, 2, 3);
template<typename F, typename... Args>
    requires (std::invocable<F, Args...> && detail::is_task_v<std::invoke_result_t<F, Args...>>)
void go_to(size_t worker_id, F&& f, Args&&... args) {
    auto* sched = runtime::scheduler::current();
    if (sched && sched->is_running()) {
        sched->go_to(worker_id, std::forward<F>(f), std::forward<Args>(args)...);
        return;
    }
    ELIO_LOG_ERROR("elio::go_to() called without a running scheduler — aborting");
    std::abort();
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
    auto* sched = runtime::scheduler::current();
    if (sched && sched->is_running()) {
        return sched->go_joinable(std::forward<F>(f), std::forward<Args>(args)...);
    }
    using T = detail::task_value_t<std::invoke_result_t<F, Args...>>;
    auto state = std::make_shared<coro::detail::join_state<T>>();
    state->set_exception(std::make_exception_ptr(
        std::logic_error("elio::spawn() called without a running scheduler")));
    state->mark_destroyed();
    return coro::join_handle<T>{std::move(state)};
}

} // namespace elio

// Macros — syntactic sugar for inline lambda coroutines.
// These capture by reference and wrap the expression in a lambda returning task.
// Use them only when every referenced object outlives the spawned task. Prefer
// elio::go / elio::spawn with an explicit lambda capture list for detached work
// that touches local state.

/// Fire-and-forget macro for inline coroutine expressions
/// WARNING: captures referenced names by reference; ensure they outlive the task.
/// Usage: ELIO_GO(some_async_operation())
#define ELIO_GO(...)    elio::go([&]() { return __VA_ARGS__; })

/// Fire-and-forget macro for inline coroutine expressions pinned to a worker
/// WARNING: captures referenced names by reference; ensure they outlive the task.
/// Usage: ELIO_GO_TO(0, some_async_operation())
#define ELIO_GO_TO(worker_id, ...)    elio::go_to(worker_id, [&]() { return __VA_ARGS__; })

/// Spawn macro for inline coroutine expressions, returns join_handle
/// WARNING: captures referenced names by reference; keep them alive until joined.
/// Usage: auto h = ELIO_SPAWN(compute_async()); auto result = co_await h;
#define ELIO_SPAWN(...) elio::spawn([&]() { return __VA_ARGS__; })
