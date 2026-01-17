#include <catch2/catch_test_macros.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/coro/task.hpp>
#include <atomic>
#include <chrono>
#include "../test_main.cpp"  // For scaled timeouts

using namespace elio::runtime;
using namespace elio::coro;
using namespace elio::test;

TEST_CASE("Dynamic thread pool growth under load", "[dynamic_threads]") {
    scheduler sched(2);
    sched.start();
    
    REQUIRE(sched.num_threads() == 2);
    
    std::atomic<int> completed{0};
    const int num_tasks = 100;
    
    auto task_func = [&]() -> task<void> {
        for (int i = 0; i < 10000; ++i) {
            volatile int x = i * i;
            (void)x;
        }
        completed.fetch_add(1);
        co_return;
    };
    
    // Spawn initial batch
    for (int i = 0; i < 50; ++i) {
        auto t = task_func();
        sched.spawn(t.release());
    }
    
    std::this_thread::sleep_for(scaled_ms(100));
    
    // Increase thread count while tasks are running
    sched.set_thread_count(6);
    REQUIRE(sched.num_threads() == 6);
    
    // Spawn more tasks
    for (int i = 50; i < num_tasks; ++i) {
        auto t = task_func();
        sched.spawn(t.release());
    }
    
    std::this_thread::sleep_for(scaled_ms(1000));
    
    REQUIRE(completed.load() == num_tasks);
    
    sched.shutdown();
}

TEST_CASE("Dynamic thread pool shrink under load", "[dynamic_threads]") {
    scheduler sched(6);
    sched.start();
    
    REQUIRE(sched.num_threads() == 6);
    
    std::atomic<int> completed{0};
    const int num_tasks = 100;
    
    auto task_func = [&]() -> task<void> {
        for (int i = 0; i < 10000; ++i) {
            volatile int x = i * i;
            (void)x;
        }
        completed.fetch_add(1);
        co_return;
    };
    
    // Spawn initial batch
    for (int i = 0; i < 50; ++i) {
        auto t = task_func();
        sched.spawn(t.release());
    }
    
    std::this_thread::sleep_for(scaled_ms(100));
    
    // Decrease thread count while tasks are running
    sched.set_thread_count(2);
    REQUIRE(sched.num_threads() == 2);
    
    // Spawn more tasks
    for (int i = 50; i < num_tasks; ++i) {
        auto t = task_func();
        sched.spawn(t.release());
    }
    
    std::this_thread::sleep_for(scaled_ms(1000));
    
    REQUIRE(completed.load() == num_tasks);
    
    sched.shutdown();
}

TEST_CASE("Multiple thread pool adjustments", "[dynamic_threads]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<int> completed{0};
    
    auto task_func = [&]() -> task<void> {
        for (int i = 0; i < 5000; ++i) {
            volatile int x = i * i;
            (void)x;
        }
        completed.fetch_add(1);
        co_return;
    };
    
    const int total_tasks = 100;
    
    // Start with 2 threads
    for (int i = 0; i < 20; ++i) {
        auto t = task_func();
        sched.spawn(t.release());
    }
    
    std::this_thread::sleep_for(scaled_ms(50));
    
    // Grow to 4
    sched.set_thread_count(4);
    REQUIRE(sched.num_threads() == 4);
    
    for (int i = 0; i < 20; ++i) {
        auto t = task_func();
        sched.spawn(t.release());
    }
    
    std::this_thread::sleep_for(scaled_ms(50));
    
    // Grow to 8
    sched.set_thread_count(8);
    REQUIRE(sched.num_threads() == 8);
    
    for (int i = 0; i < 20; ++i) {
        auto t = task_func();
        sched.spawn(t.release());
    }
    
    std::this_thread::sleep_for(scaled_ms(50));
    
    // Shrink to 4
    sched.set_thread_count(4);
    REQUIRE(sched.num_threads() == 4);
    
    for (int i = 0; i < 20; ++i) {
        auto t = task_func();
        sched.spawn(t.release());
    }
    
    std::this_thread::sleep_for(scaled_ms(50));
    
    // Shrink to 2
    sched.set_thread_count(2);
    REQUIRE(sched.num_threads() == 2);
    
    for (int i = 0; i < 20; ++i) {
        auto t = task_func();
        sched.spawn(t.release());
    }
    
    // Active wait for completion with timeout
    auto start = std::chrono::steady_clock::now();
    while (completed.load() < total_tasks) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > scaled_sec(10)) {
            break;
        }
    }
    
    REQUIRE(completed.load() == total_tasks);
    
    sched.shutdown();
}

TEST_CASE("Thread pool growth from 1 to many", "[dynamic_threads]") {
    scheduler sched(1);
    sched.start();
    
    std::atomic<int> completed{0};
    
    auto task_func = [&]() -> task<void> {
        std::this_thread::sleep_for(scaled_ms(10));
        completed.fetch_add(1);
        co_return;
    };
    
    // With 1 thread, tasks execute slowly
    for (int i = 0; i < 50; ++i) {
        auto t = task_func();
        sched.spawn(t.release());
    }
    
    std::this_thread::sleep_for(scaled_ms(100));
    int completed_with_1 = completed.load();
    
    // Increase to many threads
    sched.set_thread_count(8);
    
    std::this_thread::sleep_for(scaled_ms(200));
    int completed_with_8 = completed.load();
    
    // Should complete more tasks with more threads
    REQUIRE(completed_with_8 > completed_with_1);
    REQUIRE(completed_with_8 == 50);
    
    sched.shutdown();
}

TEST_CASE("Thread pool maintains correctness during resize", "[dynamic_threads]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<long long> counter{0};
    const int num_tasks = 100;
    const int increments = 100;
    
    auto task_func = [&]() -> task<void> {
        for (int i = 0; i < increments; ++i) {
            counter.fetch_add(1, std::memory_order_relaxed);
        }
        co_return;
    };
    
    // Spawn tasks while resizing
    std::thread spawner([&]() {
        for (int i = 0; i < num_tasks; ++i) {
            auto t = task_func();
            sched.spawn(t.release());
            
            // Resize periodically
            if (i % 10 == 0) {
                size_t new_count = (i / 10) % 2 == 0 ? 4 : 2;
                sched.set_thread_count(new_count);
            }
            
            std::this_thread::sleep_for(scaled_ms(5));
        }
    });
    
    spawner.join();
    
    std::this_thread::sleep_for(scaled_ms(500));
    
    // All increments should complete correctly
    REQUIRE(counter.load() == num_tasks * increments);
    
    sched.shutdown();
}

TEST_CASE("Thread pool resize to 0 treated as 1", "[dynamic_threads]") {
    scheduler sched(4);
    sched.start();
    
    // Try to set to 0
    sched.set_thread_count(0);
    
    // Should be clamped to 1
    REQUIRE(sched.num_threads() == 1);
    
    // Verify it still works
    std::atomic<bool> executed{false};
    auto task_func = [&]() -> task<void> {
        executed.store(true);
        co_return;
    };
    
    auto t = task_func();
    sched.spawn(t.release());
    
    std::this_thread::sleep_for(scaled_ms(100));
    
    REQUIRE(executed.load());
    
    sched.shutdown();
}

TEST_CASE("Rapid thread pool adjustments", "[dynamic_threads]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<int> completed{0};
    
    auto task_func = [&]() -> task<void> {
        completed.fetch_add(1);
        co_return;
    };
    
    const int total_tasks = 100;
    
    // Rapidly adjust thread count
    for (int iteration = 0; iteration < 10; ++iteration) {
        size_t count = 2 + (iteration % 4);
        sched.set_thread_count(count);
        
        // Spawn some tasks
        for (int i = 0; i < 10; ++i) {
            auto t = task_func();
            sched.spawn(t.release());
        }
        
        std::this_thread::sleep_for(scaled_ms(20));
    }
    
    // Active wait for completion with timeout
    auto start = std::chrono::steady_clock::now();
    while (completed.load() < total_tasks) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > scaled_sec(10)) {
            break;
        }
    }
    
    REQUIRE(completed.load() == total_tasks);
    
    sched.shutdown();
}
