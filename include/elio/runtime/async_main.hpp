#pragma once

#include "scheduler.hpp"
#include <elio/coro/task.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <exception>

namespace elio::runtime {

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
/// @param num_threads Number of worker threads (default: hardware concurrency)
/// @return The result of the task
/// 
/// Example:
/// @code
/// coro::task<int> async_main() {
///     co_return 42;
/// }
/// 
/// int main() {
///     return elio::runtime::run(async_main());
/// }
/// @endcode
template<typename T>
T run(coro::task<T> task, size_t num_threads = std::thread::hardware_concurrency()) {
    detail::completion_signal<T> signal;
    
    scheduler sched(num_threads == 0 ? 1 : num_threads);
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

} // namespace elio::runtime

namespace elio {

/// Convenience alias - run a coroutine to completion
using runtime::run;

} // namespace elio

/// Macro to define main() that runs an async_main coroutine
/// 
/// Usage:
/// @code
/// elio::coro::task<int> async_main() {
///     // Your async code here
///     co_return 0;
/// }
/// 
/// ELIO_ASYNC_MAIN(async_main)
/// @endcode
#define ELIO_ASYNC_MAIN(async_main_func) \
    int main() { \
        return elio::run(async_main_func()); \
    }

/// Macro for async_main that returns void (exits with 0)
#define ELIO_ASYNC_MAIN_VOID(async_main_func) \
    int main() { \
        elio::run(async_main_func()); \
        return 0; \
    }
