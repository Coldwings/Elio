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

TEST_CASE("cancellable sleep - normal completion", "[time][sleep][cancel]") {
    std::atomic<bool> completed{false};
    std::atomic<int> result_value{-1};
    
    cancel_source source;
    
    auto sleep_task = [&]() -> task<void> {
        auto result = co_await sleep_for(50ms, source.get_token());
        result_value = (result == cancel_result::completed) ? 1 : 0;
        completed = true;
    };
    
    scheduler sched(1);
    sched.start();
    
    {
        auto t = sleep_task();
        sched.spawn(t.release());
    }
    
    // Wait for completion without cancelling
    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    
    sched.shutdown();
    
    REQUIRE(completed);
    REQUIRE(result_value == 1);  // Should be completed, not cancelled
}

TEST_CASE("cancellable sleep - cancelled early", "[time][sleep][cancel]") {
    std::atomic<bool> completed{false};
    std::atomic<int> result_value{-1};
    
    cancel_source source;
    
    auto sleep_task = [&]() -> task<void> {
        auto start = std::chrono::steady_clock::now();
        auto result = co_await sleep_for(500ms, source.get_token());
        auto elapsed = std::chrono::steady_clock::now() - start;
        
        result_value = (result == cancel_result::cancelled) ? 1 : 0;
        completed = true;
        
        // Should have been cancelled early, not waited full 500ms
        REQUIRE(elapsed < 400ms);
    };
    
    scheduler sched(1);
    sched.start();
    
    {
        auto t = sleep_task();
        sched.spawn(t.release());
    }
    
    // Wait a bit then cancel
    std::this_thread::sleep_for(50ms);
    source.cancel();
    
    // Wait for completion
    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    
    sched.shutdown();
    
    REQUIRE(completed);
    REQUIRE(result_value == 1);  // Should be cancelled
}

TEST_CASE("cancellable sleep - already cancelled token", "[time][sleep][cancel]") {
    std::atomic<bool> completed{false};
    std::atomic<int> result_value{-1};
    
    cancel_source source;
    source.cancel();  // Cancel before sleep starts
    
    auto sleep_task = [&]() -> task<void> {
        auto start = std::chrono::steady_clock::now();
        auto result = co_await sleep_for(500ms, source.get_token());
        auto elapsed = std::chrono::steady_clock::now() - start;
        
        result_value = (result == cancel_result::cancelled) ? 1 : 0;
        completed = true;
        
        // Should complete immediately since already cancelled
        REQUIRE(elapsed < 50ms);
    };
    
    scheduler sched(1);
    sched.start();
    
    {
        auto t = sleep_task();
        sched.spawn(t.release());
    }
    
    // Wait for completion
    for (int i = 0; i < 50 && !completed; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    
    sched.shutdown();
    
    REQUIRE(completed);
    REQUIRE(result_value == 1);  // Should be cancelled
}

TEST_CASE("cancel_token basic operations", "[time][cancel]") {
    cancel_source source;
    auto token = source.get_token();
    
    REQUIRE_FALSE(token.is_cancelled());
    REQUIRE_FALSE(source.is_cancelled());
    REQUIRE(static_cast<bool>(token));  // token is truthy when not cancelled
    
    source.cancel();
    
    REQUIRE(token.is_cancelled());
    REQUIRE(source.is_cancelled());
    REQUIRE_FALSE(static_cast<bool>(token));  // token is falsy when cancelled
}

TEST_CASE("cancel_token callback invocation", "[time][cancel]") {
    cancel_source source;
    auto token = source.get_token();
    
    std::atomic<int> callback_count{0};
    
    {
        auto reg1 = token.on_cancel([&]() { callback_count++; });
        auto reg2 = token.on_cancel([&]() { callback_count++; });
        
        REQUIRE(callback_count == 0);
        
        source.cancel();
        
        REQUIRE(callback_count == 2);
    }
    
    // Registering after cancellation should invoke immediately
    auto reg3 = token.on_cancel([&]() { callback_count++; });
    REQUIRE(callback_count == 3);
}

TEST_CASE("cancel_token registration unregister", "[time][cancel]") {
    cancel_source source;
    auto token = source.get_token();
    
    std::atomic<int> callback_count{0};
    
    auto reg = token.on_cancel([&]() { callback_count++; });
    reg.unregister();  // Unregister before cancel
    
    source.cancel();
    
    REQUIRE(callback_count == 0);  // Callback should not have been invoked
}
