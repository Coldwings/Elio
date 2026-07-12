#include <catch2/catch_test_macros.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/blocking_pool.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/runtime/spawn_blocking.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/time/timer.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "../test_main.cpp"  // For scaled_ms / scaled_sec

using namespace elio::runtime;
using namespace elio::coro;
using namespace elio::time;
using namespace elio::test;
using namespace std::chrono_literals;

TEST_CASE("blocking_pool::submit rejects after shutdown",
          "[blocking_pool][shutdown]") {
    blocking_pool pool(2);
    pool.shutdown();

    std::atomic<bool> ran{false};
    bool accepted = pool.submit([&] { ran.store(true); });

    REQUIRE_FALSE(accepted);
    REQUIRE_FALSE(ran.load());
}

TEST_CASE("blocking_pool::shutdown drains pending tasks inline",
          "[blocking_pool][shutdown]") {
    // Use a 1-thread pool plus a busy-wait gate so we can guarantee a task
    // is queued (not yet picked up) at the moment shutdown begins.
    blocking_pool pool(1);

    std::atomic<bool> gate_open{false};
    std::atomic<int> ran_count{0};

    // First task: holds the only worker until we release it.
    REQUIRE(pool.submit([&] {
        while (!gate_open.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        ran_count.fetch_add(1, std::memory_order_acq_rel);
    }));

    // Second task: queued behind the first. The worker may pick it up after
    // the gate releases, OR shutdown's inline drain may run it. Either way
    // it must execute.
    REQUIRE(pool.submit([&] {
        ran_count.fetch_add(1, std::memory_order_acq_rel);
    }));

    // Kick off shutdown on a helper thread. It will block joining the worker
    // until we release the gate.
    std::thread shutdowner([&] { pool.shutdown(); });

    // Release the gate so the worker can finish.
    gate_open.store(true, std::memory_order_release);
    shutdowner.join();

    REQUIRE(ran_count.load() == 2);

    // After shutdown, further submissions are refused.
    std::atomic<bool> late{false};
    REQUIRE_FALSE(pool.submit([&] { late.store(true); }));
    REQUIRE_FALSE(late.load());
}

TEST_CASE("blocking_pool::shutdown can run from a pool worker",
          "[blocking_pool][shutdown]") {
    auto pool = std::make_unique<blocking_pool>(1);
    std::atomic<bool> shutdown_returned{false};

    REQUIRE(pool->submit([&] {
        pool->shutdown();
        shutdown_returned.store(true, std::memory_order_release);
    }));

    const auto deadline = std::chrono::steady_clock::now() + scaled_ms(5000);
    while (!shutdown_returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(scaled_ms(10));
    }

    REQUIRE(shutdown_returned.load(std::memory_order_acquire));

    // Destroying the pool after a worker-initiated shutdown must not see a
    // joinable std::thread left behind for the current worker.
    pool.reset();
}

TEST_CASE("spawn_blocking does not strand awaiter when scheduler shuts down",
          "[scheduler][shutdown][blocking_pool]") {
    // Regression for the bug where scheduler::shutdown_force() called
    // blocking_pool::shutdown() (workers exit), and a coroutine that was
    // about to call spawn_blocking on a still-running scheduler would
    // submit() onto the now-empty pool, get its task enqueued with no
    // worker to run it, and suspend forever.
    //
    // We exercise the race directly against blocking_pool: spawn many
    // submitter threads while another thread shuts the pool down. Every
    // submitted task must either (a) be accepted and run by a worker /
    // by the inline drain, or (b) be rejected and run by the caller's
    // fallback path. Nothing may be silently dropped.
    constexpr int kSubmitters = 32;
    constexpr int kPerSubmitter = 16;

    blocking_pool pool(2);
    std::atomic<int> ran{0};
    std::atomic<int> rejected{0};

    std::atomic<bool> start{false};
    std::vector<std::thread> submitters;
    submitters.reserve(kSubmitters);
    for (int t = 0; t < kSubmitters; ++t) {
        submitters.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kPerSubmitter; ++i) {
                bool accepted = pool.submit([&] {
                    ran.fetch_add(1, std::memory_order_acq_rel);
                });
                if (!accepted) {
                    // Caller-side fallback: still "runs" the work.
                    rejected.fetch_add(1, std::memory_order_acq_rel);
                    ran.fetch_add(1, std::memory_order_acq_rel);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    // Race shutdown against the submitters.
    pool.shutdown();
    for (auto& th : submitters) th.join();

    // Every submission either ran in the pool or the caller observed the
    // rejection and ran it manually. Total must be exact.
    REQUIRE(ran.load() == kSubmitters * kPerSubmitter);
    REQUIRE(rejected.load() >= 0);
}

namespace {

task<int> spawn_blocking_returning_42() {
    int v = co_await elio::spawn_blocking([] { return 42; });
    co_return v;
}

task<void> spawn_blocking_until_released(std::atomic<bool>* started,
                                         std::atomic<bool>* release,
                                         std::atomic<bool>* finished) {
    co_await elio::spawn_blocking([=] {
        started->store(true, std::memory_order_release);
        while (!release->load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        finished->store(true, std::memory_order_release);
    });
}

}  // namespace

TEST_CASE("spawn_blocking races with scheduler shutdown without hanging",
          "[scheduler][shutdown][blocking_pool]") {
    // Spawn many coroutines that each run a spawn_blocking, then call
    // shutdown_force() concurrently. Before the fix, a submit() that lost
    // the race against blocking_pool::shutdown() would enqueue work no
    // worker would run, and its awaiting coroutine would never resume
    // (active_tasks would never reach 0 within the timeout).
    constexpr int kTasks = 64;
    scheduler sched(2);
    sched.start();

    for (int i = 0; i < kTasks; ++i) {
        sched.go(spawn_blocking_returning_42);
    }

    // Force shutdown immediately. The drain inside shutdown_force first
    // blocks on the blocking_pool, and any spawn_blocking calls that race
    // it must still resolve (run or fall back to a detached thread) so
    // their callers can finish before active_tasks drains.
    REQUIRE(sched.shutdown(scaled_ms(5000)));
    REQUIRE_FALSE(sched.is_running());
}

TEST_CASE("scheduler zero blocking_threads still joins accepted blocking work",
          "[scheduler][shutdown][blocking_pool]") {
    scheduler sched(1, wait_strategy::blocking(), 0);
    sched.start();

    std::atomic<bool> work_started{false};
    std::atomic<bool> release_work{false};
    std::atomic<bool> work_finished{false};
    std::atomic<bool> shutdown_returned{false};

    sched.go(spawn_blocking_until_released,
             &work_started,
             &release_work,
             &work_finished);

    const auto started_deadline =
        std::chrono::steady_clock::now() + scaled_ms(5000);
    while (!work_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < started_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(work_started.load(std::memory_order_acquire));

    std::thread shutdowner([&] {
        sched.shutdown_force();
        shutdown_returned.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(scaled_ms(50));
    REQUIRE_FALSE(shutdown_returned.load(std::memory_order_acquire));

    release_work.store(true, std::memory_order_release);
    shutdowner.join();

    REQUIRE(shutdown_returned.load(std::memory_order_acquire));
    REQUIRE(work_finished.load(std::memory_order_acquire));
    REQUIRE_FALSE(sched.is_running());
}
