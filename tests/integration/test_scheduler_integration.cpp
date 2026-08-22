#include <catch2/catch_test_macros.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/coro/task.hpp>
#include <elio/coro/frame.hpp>
#include <atomic>
#include <chrono>
#include "../test_main.cpp"  // For scaled timeouts

using namespace elio::runtime;
using namespace elio::coro;
using namespace elio::test;

TEST_CASE("Chained coroutines integration", "[integration]") {
    scheduler sched(4);
    sched.start();
    
    std::atomic<bool> completed{false};
    std::atomic<int> final_result{0};
    
    auto inner = []() -> task<int> {
        co_return 10;
    };
    
    auto middle = [&]() -> task<int> {
        int value = co_await inner();
        co_return value * 2;
    };
    
    auto outer = [&]() -> task<void> {
        int result = co_await middle();
        final_result.store(result);
        completed.store(true);
        co_return;
    };
    
    sched.go(outer);
    
    // Wait for completion
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    REQUIRE(final_result.load() == 20);
    
    sched.shutdown();
}

TEST_CASE("Deep coroutine chain", "[integration]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<int> result{0};
    
    // Create a chain of 10 coroutines
    auto level10 = []() -> task<int> { co_return 1; };
    auto level9 = [&]() -> task<int> { int v = co_await level10(); co_return v + 1; };
    auto level8 = [&]() -> task<int> { int v = co_await level9(); co_return v + 1; };
    auto level7 = [&]() -> task<int> { int v = co_await level8(); co_return v + 1; };
    auto level6 = [&]() -> task<int> { int v = co_await level7(); co_return v + 1; };
    auto level5 = [&]() -> task<int> { int v = co_await level6(); co_return v + 1; };
    auto level4 = [&]() -> task<int> { int v = co_await level5(); co_return v + 1; };
    auto level3 = [&]() -> task<int> { int v = co_await level4(); co_return v + 1; };
    auto level2 = [&]() -> task<int> { int v = co_await level3(); co_return v + 1; };
    auto level1 = [&]() -> task<void> {
        int v = co_await level2();
        result.store(v + 1);
        co_return;
    };
    
    sched.go(level1);
    
    std::this_thread::sleep_for(scaled_ms(300));
    
    // Should be 10 (each level adds 1)
    REQUIRE(result.load() == 10);
    
    sched.shutdown();
}

TEST_CASE("Parallel independent coroutines", "[integration]") {
    scheduler sched(4);
    sched.start();
    
    const int num_tasks = 50;
    std::atomic<int> completed{0};
    std::vector<std::atomic<bool>> flags(num_tasks);
    
    for (int i = 0; i < num_tasks; ++i) {
        flags[i].store(false);
    }
    
    auto task_func = [&](int id) -> task<void> {
        // Simulate some work
        for (int j = 0; j < 100; ++j) {
            volatile int x = j * j;
            (void)x;
        }
        flags[id].store(true);
        completed.fetch_add(1);
        co_return;
    };
    
    // Spawn all tasks
    for (int i = 0; i < num_tasks; ++i) {
        sched.go([&, i]() { return task_func(i); });
    }
    
    // Wait for all to complete
    std::this_thread::sleep_for(scaled_ms(500));
    
    REQUIRE(completed.load() == num_tasks);
    
    // Verify all flags set
    for (int i = 0; i < num_tasks; ++i) {
        REQUIRE(flags[i].load());
    }
    
    sched.shutdown();
}

TEST_CASE("Mixed chain and parallel coroutines", "[integration]") {
    scheduler sched(4);
    sched.start();
    
    std::atomic<int> total{0};
    
    auto compute = [](int value) -> task<int> {
        co_return value * 2;
    };
    
    auto aggregator = [&]() -> task<void> {
        std::vector<task<int>> tasks;
        
        for (int i = 1; i <= 10; ++i) {
            auto t = compute(i);
            // We need to await each task
            int result = co_await t;
            total.fetch_add(result);
        }
        
        co_return;
    };
    
    sched.go(aggregator);
    
    std::this_thread::sleep_for(scaled_ms(300));
    
    // Sum should be 2*(1+2+...+10) = 2*55 = 110
    REQUIRE(total.load() == 110);
    
    sched.shutdown();
}

TEST_CASE("Virtual stack tracking in scheduler", "[integration]") {
    scheduler sched(2);
    sched.start();

    struct virtual_stack_observation {
        bool completed = false;
        size_t inner_depth = 0;
        size_t depth_before = 0;
        size_t depth_after = 0;
        int value = 0;
    } observation;

    auto inner = [&]() -> task<int> {
        observation.inner_depth = get_stack_depth();
        co_return 42;
    };

    auto outer = [&]() -> task<void> {
        observation.depth_before = get_stack_depth();
        observation.value = co_await inner();
        observation.depth_after = get_stack_depth();
        observation.completed = true;
        co_return;
    };

    auto handle = sched.go_joinable(outer);
    const bool drained = sched.shutdown(scaled_sec(10));
    const auto observed = observation;

    CAPTURE(drained, observed.completed, observed.inner_depth,
            observed.depth_before, observed.depth_after, observed.value);
    REQUIRE(drained);
    REQUIRE_NOTHROW(handle.await_resume());
    REQUIRE(observed.completed);
    REQUIRE(observed.inner_depth >= 1);
    REQUIRE(observed.depth_before == observed.depth_after);
    REQUIRE(observed.value == 42);
}

TEST_CASE("Scheduler load distribution", "[integration]") {
    scheduler sched(4);
    sched.start();
    
    const int num_tasks = 100;
    std::atomic<int> completed{0};
    
    auto heavy_task = [&]() -> task<void> {
        // Simulate heavy work
        for (int i = 0; i < 10000; ++i) {
            volatile int x = i * i;
            (void)x;
        }
        completed.fetch_add(1);
        co_return;
    };
    
    // Spawn all tasks at once
    for (int i = 0; i < num_tasks; ++i) {
        sched.go(heavy_task);
    }
    
    // Wait for completion
    std::this_thread::sleep_for(scaled_ms(1000));
    
    REQUIRE(completed.load() == num_tasks);
    
    // Verify work was distributed across workers
    size_t total_executed = sched.total_tasks_executed();
    REQUIRE(total_executed >= num_tasks);
    
    sched.shutdown();
}
