#include <catch2/catch_test_macros.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/runtime/spawn_blocking.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include "../test_main.cpp"  // For scaled_ms / scaled_sec

using namespace elio::runtime;
using namespace elio::coro;
using namespace elio::test;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Normal operation: spawn_blocking completes before coroutine destruction
// ---------------------------------------------------------------------------

TEST_CASE("spawn_blocking returns value normally", "[spawn_blocking]") {
    scheduler sched(2);
    sched.start();

    auto task_fn = []() -> task<int> {
        co_return co_await elio::spawn_blocking([] { return 42; });
    };

    sched.go(task_fn);
    REQUIRE(sched.shutdown(scaled_ms(5000)));
}

TEST_CASE("spawn_blocking void function works normally", "[spawn_blocking]") {
    scheduler sched(2);
    sched.start();

    std::atomic<bool> ran{false};
    auto task_fn = [&ran]() -> task<void> {
        co_await elio::spawn_blocking([&ran] { ran.store(true); });
    };

    sched.go(task_fn);
    REQUIRE(sched.shutdown(scaled_ms(5000)));
    REQUIRE(ran.load());
}

TEST_CASE("spawn_blocking propagates exceptions", "[spawn_blocking]") {
    scheduler sched(2);
    sched.start();

    std::atomic<bool> caught{false};
    auto task_fn = [&caught]() -> task<void> {
        try {
            co_await elio::spawn_blocking([] {
                throw std::runtime_error("test error");
            });
        } catch (const std::runtime_error& e) {
            if (std::string(e.what()) == "test error") {
                caught.store(true);
            }
        }
    };

    sched.go(task_fn);
    REQUIRE(sched.shutdown(scaled_ms(5000)));
    REQUIRE(caught.load());
}

// ---------------------------------------------------------------------------
// UAF regression: coroutine destroyed while blocking task is running
// ---------------------------------------------------------------------------

TEST_CASE("spawn_blocking survives coroutine destruction (non-void)",
          "[spawn_blocking][uaf]") {
    // Create a coroutine that awaits spawn_blocking with a slow function,
    // then destroy the coroutine handle while the blocking work is in flight.
    // Before the fix, the worker thread would write to the freed coroutine
    // frame's state_ member. With shared_ptr + caller_alive, it safely skips.

    std::atomic<bool> work_started{false};
    std::atomic<bool> work_finished{false};

    auto task_fn = [&work_started, &work_finished]() -> task<int> {
        co_return co_await elio::spawn_blocking([&] {
            work_started.store(true, std::memory_order_release);
            // Sleep long enough that we can destroy the coroutine first
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            work_finished.store(true, std::memory_order_release);
            return 42;
        });
    };

    // Start the coroutine manually so we control its lifetime.
    // task() creates a suspended coroutine; release() extracts the raw handle.
    auto t = task_fn();
    auto handle = elio::coro::detail::task_access::release(t);
    // Resume to start execution up to the spawn_blocking suspension point
    handle.resume();

    // Wait for the blocking work to actually start
    while (!work_started.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Destroy the coroutine while the blocking task is still running.
    // This triggers ~blocking_awaitable which sets caller_alive = false.
    handle.destroy();

    // Wait for the blocking work to finish - it should complete without
    // crashing or accessing freed memory (ASAN will catch any UAF).
    while (!work_finished.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Give the worker thread a moment to execute the post-work checks
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // If we get here without ASAN errors, the fix works.
}

TEST_CASE("spawn_blocking survives coroutine destruction (void)",
          "[spawn_blocking][uaf]") {
    std::atomic<bool> work_started{false};
    std::atomic<bool> work_finished{false};

    auto task_fn = [&work_started, &work_finished]() -> task<void> {
        co_await elio::spawn_blocking([&] {
            work_started.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            work_finished.store(true, std::memory_order_release);
        });
    };

    auto t = task_fn();
    auto handle = elio::coro::detail::task_access::release(t);
    handle.resume();

    while (!work_started.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    handle.destroy();

    while (!work_finished.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_CASE("spawn_blocking survives coroutine destruction with exception",
          "[spawn_blocking][uaf]") {
    // Same as above but the blocking function throws. The exception capture
    // path must also respect caller_alive.
    std::atomic<bool> work_started{false};
    std::atomic<bool> work_finished{false};

    auto task_fn = [&work_started, &work_finished]() -> task<int> {
        co_return co_await elio::spawn_blocking([&] {
            work_started.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            work_finished.store(true, std::memory_order_release);
            throw std::runtime_error("late error");
            return 42;
        });
    };

    auto t = task_fn();
    auto handle = elio::coro::detail::task_access::release(t);
    handle.resume();

    while (!work_started.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    handle.destroy();

    while (!work_finished.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ---------------------------------------------------------------------------
// Shutdown with pending blocking tasks
// ---------------------------------------------------------------------------

TEST_CASE("spawn_blocking handles scheduler shutdown with pending work",
          "[spawn_blocking][shutdown]") {
    // Spawn multiple coroutines that each do a slow spawn_blocking,
    // then shut down the scheduler. All tasks must resolve without hang.
    constexpr int kTasks = 8;
    scheduler sched(2);
    sched.start();

    std::atomic<int> completed{0};
    auto task_fn = [&completed]() -> task<int> {
        int v = co_await elio::spawn_blocking([&completed] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            completed.fetch_add(1, std::memory_order_acq_rel);
            return 1;
        });
        co_return v;
    };

    for (int i = 0; i < kTasks; ++i) {
        sched.go(task_fn);
    }

    REQUIRE(sched.shutdown(scaled_ms(10000)));
    REQUIRE(completed.load() == kTasks);
}

// ---------------------------------------------------------------------------
// Detached thread fallback path
// ---------------------------------------------------------------------------

TEST_CASE("spawn_blocking detached thread fallback respects caller_alive",
          "[spawn_blocking][uaf]") {
    // Force the detached-thread fallback by not having a scheduler context.
    // When there's no current scheduler, spawn_blocking falls back to
    // std::thread(...).detach(). That path must also be safe against UAF.

    std::atomic<bool> work_started{false};
    std::atomic<bool> work_finished{false};

    auto task_fn = [&work_started, &work_finished]() -> task<int> {
        co_return co_await elio::spawn_blocking([&] {
            work_started.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            work_finished.store(true, std::memory_order_release);
            return 99;
        });
    };

    // Run outside any scheduler context to trigger the detached thread path
    auto t = task_fn();
    auto handle = elio::coro::detail::task_access::release(t);
    handle.resume();

    while (!work_started.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    handle.destroy();

    while (!work_finished.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_CASE("spawn_blocking detached thread fallback completes without self-deadlock",
          "[spawn_blocking][fallback]") {
    // Force the detached-thread fallback by running outside any scheduler
    // context. The fallback worker resumes this coroutine inline; the
    // awaitable destructor must not wait for kDone on the same resume stack.
    std::atomic<bool> completed{false};
    std::atomic<int> result{0};

    auto task_fn = [&completed, &result]() -> task<void> {
        int value = co_await elio::spawn_blocking([] { return 123; });
        result.store(value, std::memory_order_release);
        completed.store(true, std::memory_order_release);
    };

    auto t = task_fn();
    auto handle = elio::coro::detail::task_access::release(t);
    handle.resume();

    const auto deadline = std::chrono::steady_clock::now() + scaled_ms(5000);
    while (!completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(scaled_ms(10));
    }

    REQUIRE(completed.load(std::memory_order_acquire));
    REQUIRE(result.load(std::memory_order_acquire) == 123);
}
