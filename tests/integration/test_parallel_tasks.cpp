#include <catch2/catch_test_macros.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/coro/task.hpp>
#include <atomic>
#include <chrono>
#include <vector>
#include "../test_main.cpp"  // For scaled timeouts

using namespace elio::runtime;
using namespace elio::coro;
using namespace elio::test;

TEST_CASE("Parallel task execution stress test", "[parallel]") {
    scheduler sched(4);
    sched.start();
    
    const int num_tasks = 500;
    std::atomic<int> completed{0};
    
    auto task_func = [&]() -> task<void> {
        // Light work
        std::this_thread::yield();
        completed.fetch_add(1);
        co_return;
    };
    
    for (int i = 0; i < num_tasks; ++i) {
        auto t = task_func();
        sched.spawn(t.release());
    }
    
    // Wait for completion with scaled timeout
    auto start = std::chrono::steady_clock::now();
    while (completed.load() < num_tasks) {
        std::this_thread::sleep_for(scaled_ms(10));
        
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > scaled_sec(5)) {
            FAIL("Timeout waiting for tasks");
        }
    }
    
    REQUIRE(completed.load() == num_tasks);
    
    sched.shutdown();
}

TEST_CASE("Parallel tasks with varying workloads", "[parallel]") {
    scheduler sched(4);
    sched.start();
    
    std::atomic<int> light_completed{0};
    std::atomic<int> heavy_completed{0};
    
    auto light_task = [&]() -> task<void> {
        light_completed.fetch_add(1);
        co_return;
    };
    
    auto heavy_task = [&]() -> task<void> {
        for (int i = 0; i < 50000; ++i) {
            volatile int x = i * i;
            (void)x;
        }
        heavy_completed.fetch_add(1);
        co_return;
    };
    
    // Mix of light and heavy tasks
    for (int i = 0; i < 100; ++i) {
        auto t = light_task();
        sched.spawn(t.release());
    }
    
    for (int i = 0; i < 20; ++i) {
        auto t = heavy_task();
        sched.spawn(t.release());
    }
    
    std::this_thread::sleep_for(scaled_ms(1000));
    
    REQUIRE(light_completed.load() == 100);
    REQUIRE(heavy_completed.load() == 20);
    
    sched.shutdown();
}

TEST_CASE("Parallel tasks with dependencies", "[parallel]") {
    scheduler sched(4);
    sched.start();
    
    std::atomic<int> stage1_completed{0};
    std::atomic<int> stage2_completed{0};
    
    auto stage1_task = [&]() -> task<int> {
        stage1_completed.fetch_add(1);
        co_return 42;
    };
    
    auto stage2_task = [&]() -> task<void> {
        auto t = stage1_task();
        int value = co_await t;
        REQUIRE(value == 42);
        stage2_completed.fetch_add(1);
        co_return;
    };
    
    const int num_chains = 50;
    for (int i = 0; i < num_chains; ++i) {
        auto t = stage2_task();
        sched.spawn(t.release());
    }
    
    // Active wait for completion with timeout
    auto start = std::chrono::steady_clock::now();
    while (stage2_completed.load() < num_chains) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > scaled_sec(10)) {
            break;
        }
    }
    
    REQUIRE(stage1_completed.load() == num_chains);
    REQUIRE(stage2_completed.load() == num_chains);
    
    sched.shutdown();
}

TEST_CASE("Work stealing under heavy load", "[parallel]") {
    scheduler sched(4);
    sched.start();
    
    const int num_tasks = 200;
    std::atomic<int> completed{0};
    std::atomic<size_t> total_steals{0};
    
    auto task_func = [&]() -> task<void> {
        // Medium work
        for (int i = 0; i < 5000; ++i) {
            volatile int x = i * i;
            (void)x;
        }
        completed.fetch_add(1);
        co_return;
    };
    
    // Spawn all tasks quickly
    for (int i = 0; i < num_tasks; ++i) {
        auto t = task_func();
        sched.spawn(t.release());
    }
    
    std::this_thread::sleep_for(scaled_ms(1500));
    
    REQUIRE(completed.load() == num_tasks);
    
    // Verify work was distributed
    size_t total_executed = sched.total_tasks_executed();
    REQUIRE(total_executed >= num_tasks);
    
    sched.shutdown();
}

TEST_CASE("Concurrent spawn and execution", "[parallel]") {
    scheduler sched(4);
    sched.start();
    
    std::atomic<int> completed{0};
    const int spawner_threads = 4;
    const int tasks_per_thread = 50;
    
    auto task_func = [&]() -> task<void> {
        completed.fetch_add(1);
        co_return;
    };
    
    std::vector<std::thread> spawners;
    for (int i = 0; i < spawner_threads; ++i) {
        spawners.emplace_back([&]() {
            for (int j = 0; j < tasks_per_thread; ++j) {
                auto t = task_func();
                sched.spawn(t.release());
                std::this_thread::yield();
            }
        });
    }
    
    for (auto& spawner : spawners) {
        spawner.join();
    }
    
    std::this_thread::sleep_for(scaled_ms(500));
    
    REQUIRE(completed.load() == spawner_threads * tasks_per_thread);
    
    sched.shutdown();
}

TEST_CASE("Parallel tasks with shared atomic counter", "[parallel]") {
    scheduler sched(4);
    sched.start();
    
    std::atomic<long long> counter{0};
    const int num_tasks = 100;
    const int increments_per_task = 1000;
    
    auto increment_task = [&]() -> task<void> {
        for (int i = 0; i < increments_per_task; ++i) {
            counter.fetch_add(1, std::memory_order_relaxed);
        }
        co_return;
    };
    
    for (int i = 0; i < num_tasks; ++i) {
        auto t = increment_task();
        sched.spawn(t.release());
    }
    
    std::this_thread::sleep_for(scaled_ms(800));
    
    REQUIRE(counter.load() == num_tasks * increments_per_task);
    
    sched.shutdown();
}

TEST_CASE("Nested parallel tasks", "[parallel]") {
    scheduler sched(4);
    sched.start();
    
    std::atomic<int> inner_completed{0};
    std::atomic<int> outer_completed{0};
    
    auto inner_task = [&]() -> task<int> {
        inner_completed.fetch_add(1);
        co_return 1;
    };
    
    auto outer_task = [&]() -> task<void> {
        int sum = 0;
        for (int i = 0; i < 5; ++i) {
            auto t = inner_task();
            int value = co_await t;
            sum += value;
        }
        REQUIRE(sum == 5);
        outer_completed.fetch_add(1);
        co_return;
    };
    
    const int num_outer = 20;
    for (int i = 0; i < num_outer; ++i) {
        auto t = outer_task();
        sched.spawn(t.release());
    }
    
    std::this_thread::sleep_for(scaled_ms(800));
    
    REQUIRE(outer_completed.load() == num_outer);
    REQUIRE(inner_completed.load() == num_outer * 5);
    
    sched.shutdown();
}
