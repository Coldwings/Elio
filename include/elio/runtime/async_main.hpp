#pragma once

#include "scheduler.hpp"
#include <elio/coro/task.hpp>
#include <atomic>
#include <condition_variable>
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
};

namespace detail {

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
template<typename T>
coro::task<void> completion_wrapper(coro::task<T> inner, completion_signal<T>* signal) {
    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(inner);
            signal->set_result();
        } else {
            T result = co_await std::move(inner);
            signal->set_result(std::move(result));
        }
    } catch (...) {
        signal->set_exception(std::current_exception());
    }
}

} // namespace detail

/// Run a coroutine task to completion and return its result
/// 
/// This function creates a scheduler, runs the given task, waits for
/// completion, and returns the result. It's the recommended way to
/// run async code from a synchronous context (like main()).
/// 
/// @param task The coroutine task to run
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
///     return elio::run(async_main());
/// }
/// @endcode
template<typename T>
T run(coro::task<T> task, const run_config& config = {}) {
    detail::completion_signal<T> signal;
    
    size_t threads = config.num_threads;
    if (threads == 0) {
        threads = std::thread::hardware_concurrency();
        if (threads == 0) threads = 1;
    }
    
    scheduler sched(threads);
    sched.start();
    
    // Create wrapper that signals completion
    auto wrapper = detail::completion_wrapper(std::move(task), &signal);
    sched.spawn(wrapper.release());
    
    // Wait for completion
    if constexpr (std::is_void_v<T>) {
        signal.wait();
        sched.shutdown();
    } else {
        T result = signal.wait();
        sched.shutdown();
        return result;
    }
}

/// Run a coroutine task with specified number of threads
template<typename T>
T run(coro::task<T> task, size_t num_threads) {
    return run(std::move(task), run_config{.num_threads = num_threads});
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
        return elio::run(async_main_func(argc, argv)); \
    }

/// Macro for async_main that returns void (exits with 0)
/// 
/// The async_main function should have signature:
///   coro::task<void> async_main(int argc, char* argv[])
#define ELIO_ASYNC_MAIN_VOID(async_main_func) \
    int main(int argc, char* argv[]) { \
        elio::run(async_main_func(argc, argv)); \
        return 0; \
    }

/// Macro for async_main without arguments
/// 
/// The async_main function should have signature:
///   coro::task<int> async_main()
#define ELIO_ASYNC_MAIN_NOARGS(async_main_func) \
    int main() { \
        return elio::run(async_main_func()); \
    }

/// Macro for async_main without arguments, returning void
/// 
/// The async_main function should have signature:
///   coro::task<void> async_main()
#define ELIO_ASYNC_MAIN_VOID_NOARGS(async_main_func) \
    int main() { \
        elio::run(async_main_func()); \
        return 0; \
    }
