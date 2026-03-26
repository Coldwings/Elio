#pragma once

#include "scheduler.hpp"
#include "blocking_pool.hpp"
#include <coroutine>
#include <exception>
#include <optional>
#include <thread>
#include <type_traits>

namespace elio {
namespace detail {

// State for non-void results
template<typename T>
struct blocking_state {
    std::optional<T> result;
    std::exception_ptr exception;
};

// State for void results (avoid std::optional<void>)
template<>
struct blocking_state<void> {
    bool completed = false;
    std::exception_ptr exception;
};

template<typename T, typename F>
class blocking_awaitable {
public:
    explicit blocking_awaitable(F&& f) : func_(std::forward<F>(f)) {}
    blocking_awaitable(blocking_awaitable&&) = default;
    blocking_awaitable(const blocking_awaitable&) = delete;
    blocking_awaitable& operator=(const blocking_awaitable&) = delete;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> caller) {
        auto* state = &state_;
        // Capture scheduler pointer to ensure we resume on the right scheduler,
        // not directly on the blocking pool thread.
        auto* sched = runtime::get_current_scheduler();
        auto work = [state, caller, sched, f = std::move(func_)]() mutable {
            try {
                if constexpr (std::is_void_v<T>) {
                    f();
                    state->completed = true;
                } else {
                    state->result.emplace(f());
                }
            } catch (...) {
                state->exception = std::current_exception();
            }
            // Resume caller via scheduler to ensure it runs on the right thread.
            // If no scheduler, fall back to direct resume (single-threaded case).
            if (sched && sched->is_running()) {
                sched->spawn(caller);
            } else if (caller && !caller.done()) {
                caller.resume();
            }
        };

        // Try blocking pool first, fallback to detached thread
        if (sched && sched->is_running()) {
            if (auto* pool = sched->get_blocking_pool()) {
                pool->submit(std::move(work));
                return;
            }
        }
        std::thread(std::move(work)).detach();
    }

    T await_resume() {
        if (state_.exception) {
            std::rethrow_exception(state_.exception);
        }
        if constexpr (std::is_void_v<T>) {
            return;
        } else {
            return std::move(*state_.result);
        }
    }

private:
    F func_;
    blocking_state<T> state_;
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
