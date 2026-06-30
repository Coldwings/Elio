#pragma once

#include "scheduler.hpp"
#include "blocking_pool.hpp"
#include <atomic>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>

namespace elio {
namespace detail {

// Shared state between the awaiting coroutine and the blocking worker thread.
// Heap-allocated via shared_ptr so it outlives the coroutine frame if the
// coroutine is destroyed while the blocking task is still running.
template<typename T>
struct blocking_state {
    std::optional<T> result;
    std::exception_ptr exception;
    // Set to false by ~blocking_awaitable when the coroutine frame is destroyed.
    // The worker thread checks this before writing results or resuming.
    std::atomic<bool> caller_alive{true};
};

// Void specialization (avoid std::optional<void>)
template<>
struct blocking_state<void> {
    bool completed = false;
    std::exception_ptr exception;
    std::atomic<bool> caller_alive{true};
};

template<typename T, typename F>
class blocking_awaitable {
public:
    explicit blocking_awaitable(F&& f) : func_(std::forward<F>(f)) {}
    blocking_awaitable(blocking_awaitable&& other) noexcept
        : func_(std::move(other.func_)), state_(std::move(other.state_)) {}
    blocking_awaitable(const blocking_awaitable&) = delete;
    blocking_awaitable& operator=(const blocking_awaitable&) = delete;

    // When the coroutine frame is destroyed (e.g., cancellation, scope exit),
    // mark the shared state so the worker thread knows not to write to freed
    // memory or resume a dead handle.
    ~blocking_awaitable() {
        if (state_) {
            state_->caller_alive.store(false, std::memory_order_release);
        }
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> caller) {
        // Allocate shared state on the heap. Both the awaitable (via member)
        // and the work lambda (via capture) hold a shared_ptr, ensuring the
        // state survives even if the coroutine frame is freed first.
        auto state = std::make_shared<blocking_state<T>>();
        state_ = state;

        // Capture scheduler pointer to ensure we resume on the right scheduler,
        // not directly on the blocking pool thread.
        auto* sched = runtime::get_current_scheduler();
        // Materialize as std::function up front so a rejected submit() leaves
        // an intact, still-callable object we can hand to a detached thread.
        // (Passing a lambda directly would move-construct a temporary
        // std::function for submit's parameter even on the rejection path,
        // moving-from our captures.)
        std::function<void()> work = [state, caller, sched, f = std::move(func_)]() mutable {
            try {
                if constexpr (std::is_void_v<T>) {
                    f();
                    if (state->caller_alive.load(std::memory_order_acquire)) {
                        state->completed = true;
                    }
                } else {
                    auto result = f();
                    if (state->caller_alive.load(std::memory_order_acquire)) {
                        state->result.emplace(std::move(result));
                    }
                }
            } catch (...) {
                if (state->caller_alive.load(std::memory_order_acquire)) {
                    state->exception = std::current_exception();
                }
            }

            // Only resume if the caller coroutine is still alive.
            if (!state->caller_alive.load(std::memory_order_acquire)) {
                return;
            }

            // Resume caller via scheduler to ensure it runs on the right thread.
            // If no scheduler, fall back to direct resume (single-threaded case).
            if (sched && sched->is_running()) {
                sched->spawn(caller);
            } else if (caller && !caller.done()) {
                caller.resume();
            }
        };

        // Try blocking pool first, fallback to detached thread.
        // submit() may refuse if the pool is already shutting down (e.g.
        // scheduler shutdown is in progress on another thread); on rejection
        // it leaves `work` untouched so we can still run it on a detached
        // thread and resume the awaiter instead of hanging forever.
        if (sched && sched->is_running()) {
            if (auto* pool = sched->get_blocking_pool()) {
                if (pool->submit(std::move(work))) {
                    return;
                }
            }
        }
        std::thread(std::move(work)).detach();
    }

    T await_resume() {
        if (state_->exception) {
            std::rethrow_exception(state_->exception);
        }
        if constexpr (std::is_void_v<T>) {
            return;
        } else {
            return std::move(*state_->result);
        }
    }

private:
    F func_;
    std::shared_ptr<blocking_state<T>> state_;
};

}  // namespace detail

/// Spawn a blocking operation on a dedicated thread pool.
/// The calling coroutine suspends until the operation completes.
/// Any exception thrown by f() is propagated to the awaiting coroutine.
///
/// Example:
///   int fd = co_await elio::spawn_blocking([&] {
///       return ::open("/path/to/file", O_RDONLY);
///   });
template<typename F>
auto spawn_blocking(F&& f) {
    using R = std::invoke_result_t<std::decay_t<F>>;
    static_assert(!std::is_reference_v<R>,
                  "spawn_blocking does not support callables returning references");
    return detail::blocking_awaitable<R, std::decay_t<F>>(std::forward<F>(f));
}

}  // namespace elio
