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

// Three-state resume claim protocol.
//
// Both the worker thread and ~blocking_awaitable contend to transition from
// kAlive.  CAS provides mutual exclusion: exactly one side wins.
//
//   kAlive ──CAS──▶ kResuming  (worker claimed resume rights)
//        └──CAS──▶ kDead       (destructor claimed — frame about to be freed)
//
// If the worker wins (alive → resuming), it must call resume and then store
// kDone so the destructor (which spin-waits on kResuming) can proceed.
// If the destructor sees kResuming, it spin-waits until kDone — the coroutine
// frame must not be freed while the worker is still touching the handle.
enum resume_state_t : int {
    kAlive    = 0,  // initial: neither side has claimed
    kResuming = 1,  // worker claimed: about to resume the caller
    kDead     = 2,  // destructor claimed: frame is being destroyed
    kDone     = 3,  // worker finished resuming; destructor may proceed
};

inline thread_local const void* inline_resuming_state = nullptr;

class inline_resume_guard {
public:
    explicit inline_resume_guard(const void* state) noexcept
        : previous_(inline_resuming_state) {
        inline_resuming_state = state;
    }

    ~inline_resume_guard() {
        inline_resuming_state = previous_;
    }

    inline_resume_guard(const inline_resume_guard&) = delete;
    inline_resume_guard& operator=(const inline_resume_guard&) = delete;

private:
    const void* previous_;
};

// Shared state between the awaiting coroutine and the blocking worker thread.
// Heap-allocated via shared_ptr so it outlives the coroutine frame if the
// coroutine is destroyed while the blocking task is still running.
template<typename T>
struct blocking_state {
    std::optional<T> result;
    std::exception_ptr exception;
    std::atomic<int> resume_state{kAlive};
};

// Void specialization (avoid std::optional<void>)
template<>
struct blocking_state<void> {
    bool completed = false;
    std::exception_ptr exception;
    std::atomic<int> resume_state{kAlive};
};

template<typename T, typename F>
class blocking_awaitable {
public:
    explicit blocking_awaitable(F&& f) : func_(std::forward<F>(f)) {}
    blocking_awaitable(blocking_awaitable&& other) noexcept
        : func_(std::move(other.func_)), state_(std::move(other.state_)) {}
    blocking_awaitable(const blocking_awaitable&) = delete;
    blocking_awaitable& operator=(const blocking_awaitable&) = delete;

    // Destructor: claim the resume right.  If the worker already claimed,
    // spin-wait until it finishes resuming — the coroutine frame must not
    // be freed while the worker is still operating on the handle.
    ~blocking_awaitable() {
        if (!state_) return;
        int expected = kAlive;
        if (state_->resume_state.compare_exchange_strong(
                expected, kDead,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            // We won — worker will see kDead and skip all writes/resume.
            return;
        }
        // Worker claimed first (state is kResuming).  Spin until it signals
        // kDone, meaning resume/spawn has completed and the handle is no
        // longer being touched.
        //
        // If this destructor is running inside the same thread's direct
        // caller.resume() fallback, waiting for kDone would self-deadlock:
        // kDone is stored only after caller.resume() returns.  In that case
        // the worker still owns the heap state and will not touch the
        // coroutine handle after resume returns, so the awaitable may finish
        // destruction without waiting.  Destruction from any other thread is
        // still blocked until kDone protects the coroutine frame.
        if (expected == kResuming && inline_resuming_state == state_.get()) {
            return;
        }
        while (state_->resume_state.load(std::memory_order_acquire) != kDone) {
            // Spin — worker is in the middle of resume and will set kDone
            // shortly (a few instructions, no blocking calls).
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
            // Execute the blocking work.  The shared_ptr keeps state alive
            // regardless of whether the coroutine frame still exists.
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

            // Atomically claim the resume right.  CAS from kAlive → kResuming.
            // This closes the TOCTOU window: if the destructor already set
            // kDead, CAS fails and we skip resume entirely.  If we win, the
            // destructor will spin-wait on kResuming until we store kDone.
            int expected = kAlive;
            if (!state->resume_state.compare_exchange_strong(
                    expected, kResuming,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                // Destructor already claimed (kDead).  Coroutine frame is
                // being freed — do NOT touch the handle.
                return;
            }

            // We claimed resume rights.  The destructor will spin-wait until
            // we store kDone, so the frame stays alive throughout this block.
            if (sched && sched->is_running()) {
                sched->spawn(caller);
            } else if (caller && !caller.done()) {
                inline_resume_guard guard(state.get());
                caller.resume();
            }

            // Signal the destructor that we're done with the handle.
            state->resume_state.store(kDone, std::memory_order_release);
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
