#include <catch2/catch_test_macros.hpp>
#include <elio/time/timer.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>

#include <chrono>
#include <atomic>

using namespace elio::time;
using namespace elio::coro;
using namespace elio::runtime;
using namespace std::chrono_literals;

TEST_CASE("sleep_for basic", "[time][sleep]") {
    std::atomic<bool> completed{false};
    
    auto sleep_task = [&]() -> task<void> {
        auto start = std::chrono::steady_clock::now();
        co_await sleep_for(50ms);
        auto elapsed = std::chrono::steady_clock::now() - start;
        
        // Should have waited approximately 50ms (allow some tolerance)
        REQUIRE(elapsed >= 40ms);
        REQUIRE(elapsed < 200ms);
        
        completed = true;
    };
    
    scheduler sched(1);
    sched.start();
    
    {
        auto t = sleep_task();
        sched.spawn(t.release());  // Transfer ownership to scheduler
    }
    
    // Wait for completion
    for (int i = 0; i < 50 && !completed; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    
    sched.shutdown();
    
    REQUIRE(completed);
}

TEST_CASE("sleep_for zero duration", "[time][sleep]") {
    std::atomic<bool> completed{false};
    
    auto sleep_task = [&]() -> task<void> {
        co_await sleep_for(0ms);  // Should complete immediately
        completed = true;
    };
    
    auto t = sleep_task();
    t.handle().resume();
    
    REQUIRE(completed);
}

TEST_CASE("yield execution", "[time][yield]") {
    std::atomic<int> counter{0};
    std::atomic<int> completed{0};
    
    auto yield_task = [&]() -> task<void> {
        for (int i = 0; i < 3; ++i) {
            counter++;
            co_await yield();
        }
        completed++;
    };
    
    scheduler sched(2);
    sched.start();
    
    {
        auto t1 = yield_task();
        auto t2 = yield_task();
        sched.spawn(t1.release());
        sched.spawn(t2.release());
    }
    
    // Wait for completion
    for (int i = 0; i < 50 && completed < 2; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    
    sched.shutdown();
    
    REQUIRE(completed == 2);
    REQUIRE(counter == 6);  // Each task increments 3 times
}

TEST_CASE("multiple sleeps sequential", "[time][sleep]") {
    std::atomic<bool> completed{false};
    
    auto multi_sleep = [&]() -> task<void> {
        auto start = std::chrono::steady_clock::now();
        
        co_await sleep_for(20ms);
        co_await sleep_for(20ms);
        co_await sleep_for(20ms);
        
        auto elapsed = std::chrono::steady_clock::now() - start;
        
        // Should have waited approximately 60ms total
        REQUIRE(elapsed >= 50ms);
        
        completed = true;
    };
    
    scheduler sched(1);
    sched.start();
    
    {
        auto t = multi_sleep();
        sched.spawn(t.release());
    }
    
    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    
    sched.shutdown();
    
    REQUIRE(completed);
}

TEST_CASE("sleep_until", "[time][sleep]") {
    std::atomic<bool> completed{false};
    
    auto sleep_until_task = [&]() -> task<void> {
        auto target = std::chrono::steady_clock::now() + 50ms;
        co_await sleep_until(target);
        
        auto now = std::chrono::steady_clock::now();
        // Should have waited until approximately the target time
        REQUIRE(now >= target - 10ms);
        
        completed = true;
    };
    
    scheduler sched(1);
    sched.start();
    
    {
        auto t = sleep_until_task();
        sched.spawn(t.release());
    }
    
    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    
    sched.shutdown();
    
    REQUIRE(completed);
}

TEST_CASE("sleep_until past time", "[time][sleep]") {
    std::atomic<bool> completed{false};
    
    auto past_sleep = [&]() -> task<void> {
        auto past = std::chrono::steady_clock::now() - 100ms;
        co_await sleep_until(past);  // Should complete immediately
        completed = true;
    };
    
    auto t = past_sleep();
    t.handle().resume();
    
    REQUIRE(completed);
}
