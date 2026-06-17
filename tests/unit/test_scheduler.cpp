#include <catch2/catch_test_macros.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/coro/task.hpp>
#include <atomic>
#include <chrono>
#include "../test_main.cpp"  // For scaled timeouts

using namespace elio::runtime;
using namespace elio::coro;
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

} // namespace

TEST_CASE("Scheduler construction", "[scheduler]") {
    scheduler sched(4);
    REQUIRE(sched.num_threads() == 4);
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
