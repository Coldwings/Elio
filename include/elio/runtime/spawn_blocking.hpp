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

// Rendezvous between the worker (blocking pool / detached thread) running the
// task and the awaiting coroutine's frame teardown. Exactly one side observes
// PENDING via an atomic exchange and "wins" the right to act:
//   - Worker wins PENDING  -> it resumes the (still-alive) caller.
//   - Destructor wins PENDING -> the frame is gone; the worker, seeing the
//     ABANDONED state it left behind, must NOT touch the dangling caller.
enum class blocking_status : int {
    pending = 0,    // task submitted, caller still suspended on the frame
    completed = 1,  // worker finished and claimed the resume
    abandoned = 2,  // caller frame destroyed before the worker finished
};

// State shared between the awaitable (owns it for await_resume) and the work
// lambda (holds a copy so it outlives a destroyed coroutine frame). Heap
// allocated via shared_ptr so writes from the worker after frame teardown are
// always valid memory, never a use-after-free.

// State for non-void results
template<typename T>
struct blocking_state {
    std::optional<T> result;
    std::exception_ptr exception;
    std::atomic<blocking_status> status{blocking_status::pending};
};

// State for void results (avoid std::optional<void>)
template<>
struct blocking_state<void> {
    bool completed = false;
    std::exception_ptr exception;
    std::atomic<blocking_status> status{blocking_status::pending};
};

template<typename T, typename F>
class blocking_awaitable {
public:
    explicit blocking_awaitable(F&& f) : func_(std::forward<F>(f)) {}
    blocking_awaitable(blocking_awaitable&&) = default;
    blocking_awaitable(const blocking_awaitable&) = delete;
    blocking_awaitable& operator=(const blocking_awaitable&) = delete;

    // When the coroutine frame is torn down (normal completion OR an external
    // handle.destroy() while still suspended on the co_await), this destructor
    // runs. Mark the shared state ABANDONED so a worker that finishes later
    // does not resume the now-dangling caller. If the worker already claimed
    // COMPLETED, this exchange just observes that and does nothing.
    ~blocking_awaitable() {
        if (state_) {
            state_->status.exchange(blocking_status::abandoned,
                                    std::memory_order_acq_rel);
        }
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> caller) {
        // Heap-allocate the result state and share ownership with the work
        // lambda. The lambda's copy keeps the state alive even if the awaiting
        // coroutine frame (and this awaitable) is destroyed before the task
        // completes, so the worker never writes through a dangling pointer.
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
        std::function<void()> work =
            [state = std::move(state), caller, sched, f = std::move(func_)]() mutable {
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
            // Claim the resume. If the caller's frame was already destroyed it
            // left ABANDONED behind; in that case we must not touch the now
            // dangling handle. Only resume when we observe PENDING (i.e. the
            // frame is still suspended waiting for us).
            blocking_status prev = state->status.exchange(
                blocking_status::completed, std::memory_order_acq_rel);
            if (prev != blocking_status::pending) {
                return;  // abandoned: caller frame gone, do not resume.
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
