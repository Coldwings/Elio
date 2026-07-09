#include <catch2/catch_test_macros.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/coro/task.hpp>
#include <elio/time/timer.hpp>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include "../test_main.cpp"  // For scaled timeouts

using namespace elio::runtime;
using namespace elio::coro;
using namespace elio::time;
using namespace elio::test;

// Standalone task functions to avoid lambda capture lifetime issues
namespace {

task<void> set_executed_task(std::atomic<bool>* executed) {
    executed->store(true);
    co_return;
}

task<void> increment_counter_task(std::atomic<int>* counter) {
    counter->fetch_add(1);
    co_return;
}

task<void> empty_task() {
    co_return;
}

task<int> return_int_task(int value) {
    co_return value;
}

// Occupies a worker thread with a plain (non-coroutine) sleep so the worker
// cannot drain its inbox while this runs. Sets `running` once it starts so the
// test can synchronize on the worker actually being busy. Does NOT register any
// async I/O, so the retiring worker's io_context stays at pending_count()==0.
task<void> occupy_worker_task(std::atomic<bool>* running,
                              std::chrono::milliseconds busy_for) {
    running->store(true, std::memory_order_release);
    std::this_thread::sleep_for(busy_for);
    co_return;
}

// Starts an async I/O (sleep_for goes through the current worker's io_context)
// and only sets `done` after it resumes. If this coroutine is resumed on a
// worker that is about to exit, the I/O binds to that worker's io_context and
// is orphaned — `done` would then never become true.
task<void> io_after_resume_task(std::chrono::milliseconds sleep_for_dur,
                                std::atomic<bool>* done) {
    co_await sleep_for(sleep_for_dur);
    done->store(true, std::memory_order_release);
}

task<void> gated_io_task(std::atomic<bool>* running,
                         std::atomic<bool>* proceed,
                         std::chrono::milliseconds sleep_for_dur,
                         std::atomic<bool>* done) {
    running->store(true, std::memory_order_release);
    while (!proceed->load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    co_await sleep_for(sleep_for_dur);
    done->store(true, std::memory_order_release);
}

} // namespace

TEST_CASE("Scheduler construction", "[scheduler]") {
    scheduler sched(4);
    REQUIRE(sched.num_threads() == 4);
    REQUIRE(!sched.is_running());
}

TEST_CASE("Scheduler construction clamps oversized thread count", "[scheduler]") {
    scheduler sched(scheduler::MAX_THREADS + 1);
    REQUIRE(sched.num_threads() == scheduler::MAX_THREADS);
    REQUIRE(!sched.is_running());
}

TEST_CASE("Scheduler start/shutdown", "[scheduler]") {
    scheduler sched(2);
    REQUIRE(!sched.is_running());
    
    sched.start();
    REQUIRE(sched.is_running());
    
    sched.shutdown();
    REQUIRE(!sched.is_running());
}

TEST_CASE("Scheduler spawn and execute simple coroutine", "[scheduler]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> executed{false};
    
    sched.go(set_executed_task, &executed);
    
    // Wait for execution
    std::this_thread::sleep_for(scaled_ms(100));
    
    REQUIRE(executed.load());
    
    sched.shutdown();
}

TEST_CASE("Scheduler pause/resume", "[scheduler]") {
    scheduler sched(2);
    sched.start();
    
    REQUIRE(!sched.is_paused());
    
    sched.pause();
    REQUIRE(sched.is_paused());
    
    sched.resume();
    REQUIRE(!sched.is_paused());
    
    sched.shutdown();
}

TEST_CASE("Scheduler spawn multiple coroutines", "[scheduler]") {
    scheduler sched(4);
    sched.start();
    
    const int num_tasks = 100;
    std::atomic<int> completed{0};
    
    // Spawn many tasks using parameter passing
    for (int i = 0; i < num_tasks; ++i) {
        sched.go(increment_counter_task, &completed);
    }
    
    // Active wait for completion with timeout
    auto start = std::chrono::steady_clock::now();
    while (completed.load() < num_tasks) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > scaled_sec(10)) {
            break;
        }
    }
    
    REQUIRE(completed.load() == num_tasks);
    
    sched.shutdown();
}

TEST_CASE("Scheduler work stealing occurs", "[scheduler]") {
    const int num_tasks = 500;
    std::atomic<int> completed{0};

    auto heavy_task = [](std::atomic<int>* ctr) -> task<void> {
        volatile int sum = 0;
        // Heavier workload to ensure tasks remain in queue long enough
        // for stealing to occur, even in optimized Release builds
        for (int j = 0; j < 200000; ++j) {
            sum = sum + j * j;
        }
        (void)sum;
        ctr->fetch_add(1);
        co_return;
    };

    // Start with 1 worker so all tasks queue on worker 0
    scheduler sched(1);
    sched.start();

    for (int i = 0; i < num_tasks; ++i) {
        sched.go(heavy_task, &completed);
    }

    // Expand to 4 workers — new workers must steal from worker 0's deque
    sched.set_thread_count(4);

    // Give new workers time to start and attempt stealing
    std::this_thread::sleep_for(scaled_ms(50));

    auto start = std::chrono::steady_clock::now();
    while (completed.load() < num_tasks) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > scaled_sec(30)) break;
    }

    REQUIRE(completed.load() == num_tasks);
    REQUIRE(sched.total_steals_executed() > 0);

    size_t workers_active = 0;
    for (size_t i = 0; i < 4; ++i) {
        if (sched.worker_tasks_executed(i) > 0) ++workers_active;
    }
    REQUIRE(workers_active >= 2);

    sched.shutdown();
}

TEST_CASE("Scheduler dynamic thread pool growth", "[scheduler]") {
    scheduler sched(2);
    REQUIRE(sched.num_threads() == 2);
    
    sched.start();
    
    // Increase threads
    sched.set_thread_count(4);

    // Give new workers time to start and attempt stealing
    std::this_thread::sleep_for(scaled_ms(50));
    REQUIRE(sched.num_threads() == 4);
    
    // Spawn some tasks
    std::atomic<int> completed{0};
    
    for (int i = 0; i < 50; ++i) {
        sched.go(increment_counter_task, &completed);
    }
    
    std::this_thread::sleep_for(scaled_ms(200));
    REQUIRE(completed.load() == 50);
    
    sched.shutdown();
}

TEST_CASE("Scheduler dynamic thread pool shrink", "[scheduler]") {
    scheduler sched(4);
    REQUIRE(sched.num_threads() == 4);
    
    sched.start();
    
    // Decrease threads
    sched.set_thread_count(2);
    REQUIRE(sched.num_threads() == 2);
    
    // Spawn some tasks
    std::atomic<int> completed{0};
    
    for (int i = 0; i < 50; ++i) {
        sched.go(increment_counter_task, &completed);
    }
    
    std::this_thread::sleep_for(scaled_ms(200));
    REQUIRE(completed.load() == 50);
    
    sched.shutdown();
}

TEST_CASE("Scheduler statistics", "[scheduler]") {
    scheduler sched(2);
    sched.start();
    
    const int num_tasks = 20;
    std::atomic<int> completed{0};
    
    for (int i = 0; i < num_tasks; ++i) {
        sched.go(increment_counter_task, &completed);
    }
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load() == num_tasks);
    REQUIRE(sched.total_tasks_executed() >= num_tasks);
    
    sched.shutdown();
}

TEST_CASE("Scheduler thread-local current", "[scheduler]") {
    scheduler sched(2);
    REQUIRE(scheduler::current() == nullptr);
    
    sched.start();
    REQUIRE(scheduler::current() == &sched);
    
    sched.shutdown();
}

TEST_CASE("Scheduler handles empty spawn", "[scheduler]") {
    scheduler sched(2);
    sched.start();
    
    // Should not crash
    sched.spawn(nullptr);
    
    std::this_thread::sleep_for(scaled_ms(50));
    
    sched.shutdown();
}

TEST_CASE("Scheduler handles spawn before start", "[scheduler]") {
    scheduler sched(2);

    // Should not crash, but task won't execute (scheduler not running)
    sched.go(empty_task);

    // Now start - but the task was already queued
    sched.start();
    std::this_thread::sleep_for(scaled_ms(100));
    sched.shutdown();
}

TEST_CASE("go_joinable before start returns completed exception",
          "[scheduler][join_handle]") {
    scheduler sched(2);

    auto handle = sched.go_joinable(return_int_task, 42);

    REQUIRE(handle.is_ready());
    REQUIRE(handle.is_destroyed());
    REQUIRE_THROWS_AS(handle.await_resume(), std::logic_error);
}

TEST_CASE("go_joinable_to before start returns completed exception",
          "[scheduler][join_handle]") {
    scheduler sched(2);

    auto handle = sched.go_joinable_to(0, return_int_task, 42);

    REQUIRE(handle.is_ready());
    REQUIRE(handle.is_destroyed());
    REQUIRE_THROWS_AS(handle.await_resume(), std::logic_error);
}

TEST_CASE("go_joinable after shutdown returns completed exception",
          "[scheduler][join_handle]") {
    scheduler sched(2);
    sched.start();
    REQUIRE(sched.shutdown(scaled_ms(1000)));

    auto handle = sched.go_joinable(return_int_task, 42);

    REQUIRE(handle.is_ready());
    REQUIRE(handle.is_destroyed());
    REQUIRE_THROWS_AS(handle.await_resume(), std::logic_error);
}

TEST_CASE("go_joinable_to after shutdown returns completed exception",
          "[scheduler][join_handle]") {
    scheduler sched(2);
    sched.start();
    REQUIRE(sched.shutdown(scaled_ms(1000)));

    auto handle = sched.go_joinable_to(0, return_int_task, 42);

    REQUIRE(handle.is_ready());
    REQUIRE(handle.is_destroyed());
    REQUIRE_THROWS_AS(handle.await_resume(), std::logic_error);
}

// Regression test for the "shrink orphans I/O started by retired workers"
// bug: when set_thread_count() retires a worker, any task still queued on
// that worker must NOT start fresh async I/O on the retiring worker's
// io_context (which stops being polled once the worker exits). The fix
// redistributes such queued tasks to a surviving worker during the drain
// phase instead of resuming them locally.
//
// Setup that deterministically forces the bug's preconditions:
//   1. Pin an occupier task to worker 1 that busies the thread with a plain
//      sleep. While worker 1 is inside run_task() it cannot drain its inbox.
//   2. Once the occupier is confirmed running, pin an I/O-starting task to
//      worker 1. Because worker 1 is busy, this task sits in worker 1's MPSC
//      inbox. It cannot be stolen (stealing only touches the deque), so it
//      stays pinned to the retiring worker.
//   3. Call set_thread_count(1). The scheduler lowers the thread count, sees
//      worker 1's io_context has 0 pending ops (the occupier does no async
//      I/O and the I/O task hasn't run yet), then stop()s + joins worker 1.
//   4. The join blocks until the occupier's sleep ends. Worker 1 then enters
//      its drain phase, moves the I/O task from inbox to deque, and pops it.
//
// Without the fix: worker 1 resumes the I/O task locally, its co_await
// sleep_for binds to worker 1's (soon-orphaned) io_context, worker 1 exits,
// and the sleep never fires — `io_done` stays false.
// With the fix: worker 1 redistributes the task to worker 0, which polls its
// I/O to completion — `io_done` becomes true.
TEST_CASE("Scheduler shrink does not orphan I/O from a retiring worker",
          "[scheduler]") {
    scheduler sched(2);
    sched.start();

    std::atomic<bool> occupier_running{false};
    std::atomic<bool> io_done{false};

    // (1) Occupy worker 1 for long enough that we can queue the I/O task and
    // trigger the shrink while it is still busy.
    sched.go_to(1, occupy_worker_task, &occupier_running, scaled_ms(300));

    // (2) Wait until worker 1 is genuinely busy running the occupier.
    auto deadline = std::chrono::steady_clock::now() + scaled_sec(5);
    while (!occupier_running.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(occupier_running.load(std::memory_order_acquire));

    // (3) Queue an I/O-starting task pinned to the (now busy) worker 1. It
    // parks in worker 1's inbox because worker 1 can't drain while running the
    // occupier.
    sched.go_to(1, io_after_resume_task, scaled_ms(40), &io_done);

    // (4) Shrink to a single worker. This retires worker 1 while the I/O task
    // is still sitting in its inbox. Not called from a worker thread, so it is
    // allowed to join.
    sched.set_thread_count(1);
    REQUIRE(sched.num_threads() == 1);

    // The redistributed I/O task must complete on the surviving worker.
    auto io_deadline = std::chrono::steady_clock::now() + scaled_sec(5);
    while (!io_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < io_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(io_done.load(std::memory_order_acquire));

    sched.shutdown();
}

TEST_CASE("Scheduler shrink drains I/O started after retirement begins",
          "[scheduler]") {
    scheduler sched(2);
    sched.start();

    std::atomic<bool> task_running{false};
    std::atomic<bool> proceed{false};
    std::atomic<bool> io_done{false};

    sched.go_to(1, gated_io_task, &task_running, &proceed,
                scaled_ms(40), &io_done);

    auto running_deadline = std::chrono::steady_clock::now() + scaled_sec(5);
    while (!task_running.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < running_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(task_running.load(std::memory_order_acquire));

    std::exception_ptr shrink_error;
    std::thread shrinker([&] {
        try {
            sched.set_thread_count(1);
        } catch (...) {
            shrink_error = std::current_exception();
        }
    });

    auto shrink_deadline = std::chrono::steady_clock::now() + scaled_sec(5);
    while (sched.num_threads(std::memory_order_acquire) != 1 &&
           std::chrono::steady_clock::now() < shrink_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool shrink_visible = (sched.num_threads(std::memory_order_acquire) == 1);

    // Give the old shrink path time to reach stop()+join while the retiring
    // worker is still inside the gated task and has no pending I/O yet.
    if (shrink_visible) {
        std::this_thread::sleep_for(scaled_ms(50));
    }
    proceed.store(true, std::memory_order_release);

    shrinker.join();
    if (shrink_error) {
        std::rethrow_exception(shrink_error);
    }
    REQUIRE(shrink_visible);

    auto io_deadline = std::chrono::steady_clock::now() + scaled_sec(5);
    while (!io_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < io_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(io_done.load(std::memory_order_acquire));

    sched.shutdown();
}
