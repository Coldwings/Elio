// Regression tests for the use-after-free in spawn_blocking's detached-thread
// fallback path (issue: spawn_blocking: use-after-free in detached thread
// fallback path).
//
// Before the fix, blocking_awaitable captured a raw pointer to its `state_`
// member, which lives in the awaiting coroutine frame. If the coroutine frame
// was destroyed (e.g. handle.destroy() during shutdown / cancellation) while
// the blocking task was still running on a detached thread, the thread later
// wrote to the freed memory (write-after-free) and called resume()/done() on a
// destroyed coroutine handle (UB).
//
// The fix heap-allocates the state via std::shared_ptr (captured by value in
// the work lambda, so it outlives the frame) and uses an atomic rendezvous so
// the worker never touches a caller whose frame has already been torn down.
//
// These tests drive a coroutine manually (extract the handle, resume to the
// suspend point, then destroy it before the blocking task finishes) so the
// detached-thread path is exercised deterministically. They are most valuable
// under AddressSanitizer, which flags the original write-after-free.

#include <catch2/catch_test_macros.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/runtime/spawn_blocking.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include "../test_main.cpp"  // For scaled_ms / scaled_sec

using namespace elio::coro;
using namespace elio::test;
using namespace std::chrono_literals;

namespace {

// Shared, heap-allocated controls so they outlive the coroutine frame we are
// about to destroy. The coroutine and the blocking task only touch these via
// shared_ptr, never via the (soon to be freed) frame.
struct uaf_controls {
    std::atomic<bool> work_started{false};   // set when the blocking task begins
    std::atomic<bool> may_finish{false};     // gate: blocking task waits on this
    std::atomic<bool> work_finished{false};  // set after the blocking task writes its result
    std::atomic<bool> caller_resumed{false}; // set if the coroutine resumes past the co_await
};

// A coroutine that runs a blocking task and, if resumed, records that it got
// past the suspension point. With no running scheduler in scope,
// spawn_blocking falls back to a detached std::thread.
task<void> uaf_probe_task(std::shared_ptr<uaf_controls> ctl) {
    int v = co_await elio::spawn_blocking([ctl]() -> int {
        ctl->work_started.store(true, std::memory_order_release);
        // Spin until the test tells us to finish, so the test can destroy the
        // coroutine frame while we are still running.
        while (!ctl->may_finish.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ctl->work_finished.store(true, std::memory_order_release);
        return 42;
    });
    // If we ever get here the coroutine was resumed after the co_await.
    (void)v;
    ctl->caller_resumed.store(true, std::memory_order_release);
}

}  // namespace

TEST_CASE("spawn_blocking detached fallback: destroying frame mid-task is safe",
          "[blocking_pool][spawn_blocking][uaf]") {
    auto ctl = std::make_shared<uaf_controls>();

    // Start the coroutine and run it to the spawn_blocking suspension point.
    // No scheduler is running here, so spawn_blocking takes the detached-thread
    // fallback (std::thread(work).detach()).
    auto t = uaf_probe_task(ctl);
    auto h = elio::coro::detail::task_access::release(t);
    h.resume();  // suspends inside await_suspend; spawns the detached worker

    // Wait until the detached worker is actually running (and thus holding a
    // shared_ptr copy of the state), so destroying the frame races a live task.
    const auto deadline = std::chrono::steady_clock::now() + scaled_sec(5);
    while (!ctl->work_started.load(std::memory_order_acquire)) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Destroy the coroutine frame while the blocking task is still spinning.
    // Old behavior: the detached thread later writes to the freed `state_` and
    // calls resume()/done() on this destroyed handle. New behavior: the heap
    // state survives and the rendezvous prevents touching the dead handle.
    h.destroy();

    // Release the gate so the blocking task finishes and writes its result.
    ctl->may_finish.store(true, std::memory_order_release);

    // The task must complete (proving it ran to the write) without crashing or
    // tripping ASAN on the freed frame.
    const auto finish_deadline = std::chrono::steady_clock::now() + scaled_sec(5);
    while (!ctl->work_finished.load(std::memory_order_acquire)) {
        REQUIRE(std::chrono::steady_clock::now() < finish_deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Give the detached thread a moment to run its post-write resume logic. It
    // must observe the abandoned state and NOT resume the destroyed coroutine.
    std::this_thread::sleep_for(scaled_ms(100));
    REQUIRE_FALSE(ctl->caller_resumed.load(std::memory_order_acquire));
}

TEST_CASE("spawn_blocking detached fallback: normal completion resumes caller",
          "[blocking_pool][spawn_blocking][uaf]") {
    // Sanity check that the fix does not break the happy path: with no frame
    // destruction, the detached worker resumes the caller and the coroutine
    // runs to completion.
    auto ctl = std::make_shared<uaf_controls>();

    auto t = uaf_probe_task(ctl);
    auto h = elio::coro::detail::task_access::release(t);
    h.resume();  // suspends inside await_suspend; spawns the detached worker

    // Wait for the worker to start, then release the gate. Opening the gate
    // only after h.resume() has returned guarantees the main thread is no
    // longer touching the frame before the detached worker resumes it.
    const auto start_deadline = std::chrono::steady_clock::now() + scaled_sec(5);
    while (!ctl->work_started.load(std::memory_order_acquire)) {
        REQUIRE(std::chrono::steady_clock::now() < start_deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ctl->may_finish.store(true, std::memory_order_release);

    // The worker resumes the caller on completion. Because the task was
    // released as detached, reaching final suspend self-destructs the frame,
    // so we must NOT touch `h` again here; observe completion via our flags.
    const auto deadline = std::chrono::steady_clock::now() + scaled_sec(5);
    while (!ctl->caller_resumed.load(std::memory_order_acquire)) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(ctl->work_finished.load(std::memory_order_acquire));
    REQUIRE(ctl->caller_resumed.load(std::memory_order_acquire));
}
