#pragma once

#include "scheduler.hpp"
#include <elio/coro/task.hpp>
#include <elio/coro/vthread_stack.hpp>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <exception>
#include <span>
#include <string_view>

namespace elio::runtime {

/// Configuration for running async tasks
struct run_config {
    /// Number of worker threads (0 = hardware concurrency)
    size_t num_threads = 0;
    /// Blocking thread pool size (0 = fallback to std::thread per task)
    size_t blocking_threads = 4;
};

namespace detail {

/// Type alias using definitions from scheduler.hpp
template<typename T> using task_value_t = typename task_value<T>::type;

/// Completion signal for async_main
template<typename T>
struct completion_signal {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<T> result;
    std::exception_ptr exception;
    bool completed = false;
    
    void set_result(T value) {
        std::lock_guard<std::mutex> lock(mutex);
        result = std::move(value);
        completed = true;
        cv.notify_one();
    }
    
    void set_exception(std::exception_ptr e) {
        std::lock_guard<std::mutex> lock(mutex);
        exception = e;
        completed = true;
        cv.notify_one();
    }
    
    T wait() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return completed; });
        if (exception) {
            std::rethrow_exception(exception);
        }
        return std::move(*result);
    }
};

template<>
struct completion_signal<void> {
    std::mutex mutex;
    std::condition_variable cv;
    std::exception_ptr exception;
    bool completed = false;
    
    void set_result() {
        std::lock_guard<std::mutex> lock(mutex);
        completed = true;
        cv.notify_one();
    }
    
    void set_exception(std::exception_ptr e) {
        std::lock_guard<std::mutex> lock(mutex);
        exception = e;
        completed = true;
        cv.notify_one();
    }
    
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return completed; });
        if (exception) {
            std::rethrow_exception(exception);
        }
    }
};

/// Wrapper task that signals completion
template<typename T, typename F>
coro::task<void> completion_wrapper(F f, completion_signal<T>* signal) {
    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::invoke(std::move(f));
            signal->set_result();
        } else {
            signal->set_result(co_await std::invoke(std::move(f)));
        }
    } catch (...) {
        signal->set_exception(std::current_exception());
    }
}

} // namespace detail

/// Run a callable that returns a coroutine task to completion
/// 
/// This function creates a scheduler, runs the given task, waits for
/// completion, and returns the result. It's the recommended way to
/// run async code from a synchronous context (like main()).
/// 
/// @param f The callable that returns a coroutine task
/// @param config Configuration (threads)
/// @return The result of the task
/// 
/// Example:
/// @code
/// coro::task<int> async_main() {
///     // Your async code here - each worker has its own io_context
///     co_return 42;
/// }
/// 
/// int main() {
///     return elio::run(async_main);
/// }
/// @endcode

/// Overload 1: no-arg callable + optional config
template<typename F>
    requires (std::invocable<F> && detail::is_task_v<std::invoke_result_t<F>>)
auto run(F&& f, const run_config& config = {})
    -> detail::task_value_t<std::invoke_result_t<F>>
{
    using T = detail::task_value_t<std::invoke_result_t<F>>;
    detail::completion_signal<T> signal;

    size_t threads = config.num_threads;
    if (threads == 0) {
        threads = std::thread::hardware_concurrency();
        if (threads == 0) threads = 1;
    }

    scheduler sched(threads, wait_strategy::blocking(),
                    config.blocking_threads);
    sched.start();

    // Wrap user function
    auto bound = [&f]() { return std::invoke(std::forward<F>(f)); };

    {
        // Create root vthread_stack and set as current context
        auto* root_vstack = new coro::vthread_stack();
        auto* old_vstack = coro::vthread_stack::current();
        coro::vthread_stack::set_current(root_vstack);
        
        // Construct coroutine (will allocate from root_vstack)
        auto wrapper = detail::completion_wrapper<T>(std::move(bound), &signal);
        
        // Restore previous context (likely nullptr)
        coro::vthread_stack::set_current(old_vstack);
        
        auto handle = coro::detail::task_access::release(wrapper);
        handle.promise().set_vstack_owner(root_vstack);
        sched.spawn(handle);
    }

    if constexpr (std::is_void_v<T>) {
        signal.wait();
        sched.shutdown();
    } else {
        T result = signal.wait();
        sched.shutdown();
        return result;
    }
}

/// Overload 2: (func, args...) with config first
template<typename F, typename... Args>
    requires (sizeof...(Args) > 0 && std::invocable<F, Args...> && detail::is_task_v<std::invoke_result_t<F, Args...>>)
auto run(const run_config& config, F&& f, Args&&... args)
    -> detail::task_value_t<std::invoke_result_t<F, Args...>>
{
    auto bound = [f = std::forward<F>(f),
                  ...args = std::forward<Args>(args)]() mutable {
        return std::invoke(std::move(f), std::move(args)...);
    };
    return run(std::move(bound), config);
}

/// Overload 3: (func, args...) without config
template<typename F, typename Arg0, typename... Args>
    requires (!std::is_same_v<std::decay_t<Arg0>, run_config> && std::invocable<F, Arg0, Args...> && detail::is_task_v<std::invoke_result_t<F, Arg0, Args...>>)
auto run(F&& f, Arg0&& arg0, Args&&... args)
    -> detail::task_value_t<std::invoke_result_t<F, Arg0, Args...>>
{
    return run(run_config{}, std::forward<F>(f), std::forward<Arg0>(arg0), std::forward<Args>(args)...);
}

} // namespace elio::runtime

namespace elio {

/// Convenience alias - run a coroutine to completion
using runtime::run;

/// Convenience alias for run configuration
using runtime::run_config;

} // namespace elio

/// Macro to define main() that runs an async_main coroutine with argc/argv
/// 
/// The async_main function should have signature:
///   coro::task<int> async_main(int argc, char* argv[])
/// 
/// Usage:
/// @code
/// elio::coro::task<int> async_main(int argc, char* argv[]) {
///     if (argc < 2) {
///         std::cerr << "Usage: " << argv[0] << " <arg>\n";
///         co_return 1;
///     }
///     // Your async code here
///     co_return 0;
/// }
/// 
/// ELIO_ASYNC_MAIN(async_main)
/// @endcode
#define ELIO_ASYNC_MAIN(async_main_func) \
    int main(int argc, char* argv[]) { \
        return elio::run(async_main_func, argc, argv); \
    }

/// Macro for async_main that returns void (exits with 0)
/// 
/// The async_main function should have signature:
///   coro::task<void> async_main(int argc, char* argv[])
#define ELIO_ASYNC_MAIN_VOID(async_main_func) \
    int main(int argc, char* argv[]) { \
        elio::run(async_main_func, argc, argv); \
        return 0; \
    }

/// Macro for async_main without arguments
/// 
/// The async_main function should have signature:
///   coro::task<int> async_main()
#define ELIO_ASYNC_MAIN_NOARGS(async_main_func) \
    int main() { \
        return elio::run(async_main_func); \
    }

/// Macro for async_main without arguments, returning void
/// 
/// The async_main function should have signature:
///   coro::task<void> async_main()
#define ELIO_ASYNC_MAIN_VOID_NOARGS(async_main_func) \
    int main() { \
        elio::run(async_main_func); \
        return 0; \
    }
