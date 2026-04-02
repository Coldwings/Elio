#include <catch2/catch_test_macros.hpp>
#include <elio/coro/generator.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/time/timer.hpp>
#include <string>
#include <vector>
#include <atomic>
#include "../test_main.cpp"  // For scaled timeouts

using namespace elio::coro;
using namespace elio::runtime;
using namespace elio::test;
using namespace elio::time;

// ============================================================================
// Basic generator tests
// ============================================================================

generator<int> simple_producer() {
    co_yield 1;
    co_yield 2;
    co_yield 3;
}

TEST_CASE("generator basic usage", "[generator]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    std::vector<int> values;
    
    auto consumer = [&]() -> task<void> {
        auto gen = simple_producer();
        
        while (auto v = co_await gen.next()) {
            values.push_back(*v);
        }
        completed.store(true);
    };
    
    elio::go(consumer);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    REQUIRE(values == std::vector<int>{1, 2, 3});
    
    sched.shutdown();
}

generator<int> empty_producer() {
    co_return;
}

TEST_CASE("generator empty", "[generator]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    int count = 0;
    
    auto consumer = [&]() -> task<void> {
        auto gen = empty_producer();
        
        while (auto v = co_await gen.next()) {
            ++count;
        }
        completed.store(true);
    };
    
    elio::go(consumer);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    REQUIRE(count == 0);
    
    sched.shutdown();
}

// ============================================================================
// Async operations during yield
// ============================================================================

generator<int> producer_with_delay() {
    for (int i = 0; i < 5; ++i) {
        co_await sleep_for(std::chrono::milliseconds(20));
        co_yield i;
    }
}

TEST_CASE("generator with async delays", "[generator]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    std::vector<int> values;
    
    auto start = std::chrono::steady_clock::now();
    
    auto consumer = [&]() -> task<void> {
        auto gen = producer_with_delay();
        
        while (auto v = co_await gen.next()) {
            values.push_back(*v);
        }
        completed.store(true);
    };
    
    elio::go(consumer);
    
    std::this_thread::sleep_for(scaled_ms(500));
    
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    
    REQUIRE(completed.load());
    REQUIRE(values == std::vector<int>{0, 1, 2, 3, 4});
    // Should have taken at least 5 * 20ms = 100ms
    REQUIRE(elapsed.count() >= 80);
    
    sched.shutdown();
}

// ============================================================================
// Fibonacci with generator
// ============================================================================

generator<int> fibonacci(int n) {
    int a = 0, b = 1;
    for (int i = 0; i < n; ++i) {
        co_yield a;
        int tmp = a;
        a = b;
        b = tmp + b;
    }
}

TEST_CASE("generator fibonacci", "[generator]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    std::vector<int> values;
    
    auto consumer = [&]() -> task<void> {
        auto gen = fibonacci(10);
        
        while (auto v = co_await gen.next()) {
            values.push_back(*v);
        }
        completed.store(true);
    };
    
    elio::go(consumer);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    REQUIRE(values == std::vector<int>{0, 1, 1, 2, 3, 5, 8, 13, 21, 34});
    
    sched.shutdown();
}

// ============================================================================
// String type
// ============================================================================

generator<std::string> string_producer() {
    co_yield "hello";
    co_yield "async";
    co_yield "world";
}

TEST_CASE("generator with strings", "[generator]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    std::vector<std::string> values;
    
    auto consumer = [&]() -> task<void> {
        auto gen = string_producer();
        
        while (auto v = co_await gen.next()) {
            values.push_back(*v);
        }
        completed.store(true);
    };
    
    elio::go(consumer);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    REQUIRE(values == std::vector<std::string>{"hello", "async", "world"});
    
    sched.shutdown();
}

// ============================================================================
// Exception handling
// ============================================================================

generator<int> throwing_producer() {
    co_yield 1;
    co_yield 2;
    throw std::runtime_error("generator error");
    co_yield 3;  // Never reached
}

TEST_CASE("generator throws exception", "[generator]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> caught{false};
    std::vector<int> values;
    
    auto consumer = [&]() -> task<void> {
        auto gen = throwing_producer();
        
        try {
            while (auto v = co_await gen.next()) {
                values.push_back(*v);
            }
        } catch (const std::runtime_error& e) {
            caught.store(true);
        }
    };
    
    elio::go(consumer);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(caught.load());
    REQUIRE(values == std::vector<int>{1, 2});
    
    sched.shutdown();
}

// ============================================================================
// Single consumer
// ============================================================================

TEST_CASE("generator single consumer", "[generator]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<int> total{0};
    
    auto consumer = [&]() -> task<void> {
        auto gen = simple_producer();
        
        while (auto v = co_await gen.next()) {
            total += *v;
        }
    };
    
    elio::go(consumer);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(total.load() == 6);  // 1 + 2 + 3
    
    sched.shutdown();
}

// ============================================================================
// Producer consumer timing
// ============================================================================

generator<int> slow_producer(int delay_ms) {
    for (int i = 1; i <= 3; ++i) {
        co_await sleep_for(std::chrono::milliseconds(delay_ms));
        co_yield i;
    }
}

TEST_CASE("generator producer consumer timing", "[generator]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    std::vector<int> values;
    std::vector<std::chrono::steady_clock::time_point> timestamps;
    
    auto consumer = [&]() -> task<void> {
        auto gen = slow_producer(30);
        
        while (auto v = co_await gen.next()) {
            timestamps.push_back(std::chrono::steady_clock::now());
            values.push_back(*v);
        }
        completed.store(true);
    };
    
    auto start = std::chrono::steady_clock::now();
    elio::go(consumer);
        
    std::this_thread::sleep_for(scaled_ms(300));
        
    REQUIRE(completed.load());
    REQUIRE(values == std::vector<int>{1, 2, 3});
    
    // Check timing - each value should arrive ~30ms apart
    for (size_t i = 1; i < timestamps.size(); ++i) {
        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamps[i] - timestamps[i-1]);
        REQUIRE(diff.count() >= 20);  // Allow some tolerance
    }
    (void)start;  // Suppress unused variable warning
    
    sched.shutdown();
}

// ============================================================================
// Large values
// ============================================================================

struct LargeValue {
    std::array<int, 100> data;
    
    LargeValue() { data.fill(0); }
    explicit LargeValue(int v) { data.fill(v); }
    int sum() const { 
        int s = 0; 
        for (int v : data) s += v; 
        return s; 
    }
};

generator<LargeValue> large_producer() {
    co_yield LargeValue(1);
    co_yield LargeValue(2);
    co_yield LargeValue(3);
}

TEST_CASE("generator with large values", "[generator]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    std::vector<int> sums;
    
    auto consumer = [&]() -> task<void> {
        auto gen = large_producer();
        
        while (auto v = co_await gen.next()) {
            sums.push_back(v->sum());
        }
        completed.store(true);
    };
    
    elio::go(consumer);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    REQUIRE(sums == std::vector<int>{100, 200, 300});
    
    sched.shutdown();
}

// ============================================================================
// Virtual stack integration
// ============================================================================

TEST_CASE("generator virtual stack allocation", "[generator][vstack]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    
    auto consumer = [&]() -> task<void> {
        auto gen = simple_producer();
        
        while (auto v = co_await gen.next()) {
            // Just consume values
        }
        completed.store(true);
    };
    
    elio::go(consumer);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    
    sched.shutdown();
}

// ============================================================================
// Nested generators
// ============================================================================

generator<int> gen_nested_inner(int n) {
    for (int i = 0; i < n; ++i) {
        co_yield i;
    }
}

generator<int> gen_nested_outer() {
    auto inner1 = gen_nested_inner(3);
    auto inner2 = gen_nested_inner(2);
    
    while (auto v = co_await inner1.next()) {
        co_yield *v + 100;
    }
    
    while (auto v = co_await inner2.next()) {
        co_yield *v + 200;
    }
}

TEST_CASE("generator nested", "[generator]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    std::vector<int> values;
    
    auto consumer = [&]() -> task<void> {
        auto gen = gen_nested_outer();
        
        while (auto v = co_await gen.next()) {
            values.push_back(*v);
        }
        completed.store(true);
    };
    
    elio::go(consumer);
    
    std::this_thread::sleep_for(scaled_ms(300));
    
    REQUIRE(completed.load());
    REQUIRE(values == std::vector<int>{100, 101, 102, 200, 201});
    
    sched.shutdown();
}

// ============================================================================
// Early termination
// ============================================================================

TEST_CASE("generator early termination", "[generator]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    std::vector<int> values;
    
    auto consumer = [&]() -> task<void> {
        auto gen = fibonacci(100);
        
        int count = 0;
        while (auto v = co_await gen.next()) {
            values.push_back(*v);
            if (++count >= 5) break;  // Stop after 5 values
        }
        completed.store(true);
    };
    
    elio::go(consumer);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    REQUIRE(values == std::vector<int>{0, 1, 1, 2, 3});
    
    sched.shutdown();
}

// ============================================================================
// Infinite generator (limited by consumer)
// ============================================================================

generator<int> infinite_producer() {
    int i = 0;
    while (true) {
        co_yield i++;
    }
}

TEST_CASE("generator infinite limited by consumer", "[generator]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    std::vector<int> values;
    
    auto consumer = [&]() -> task<void> {
        auto gen = infinite_producer();
        
        int count = 0;
        while (auto v = co_await gen.next()) {
            values.push_back(*v);
            if (++count >= 10) break;
        }
        completed.store(true);
    };
    
    elio::go(consumer);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    REQUIRE(values.size() == 10);
    
    sched.shutdown();
}

// ============================================================================
// ELIO_CO_FOR macro
// ============================================================================

TEST_CASE("generator ELIO_CO_FOR macro", "[generator]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    std::vector<int> values;
    
    auto consumer = [&]() -> task<void> {
        auto gen = simple_producer();
        
        ELIO_CO_FOR(v, gen) {
            values.push_back(v);
        }
        completed.store(true);
    };
    
    elio::go(consumer);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    REQUIRE(values == std::vector<int>{1, 2, 3});
    
    sched.shutdown();
}

TEST_CASE("generator ELIO_CO_FOR with break", "[generator]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    std::vector<int> values;
    
    auto consumer = [&]() -> task<void> {
        auto gen = fibonacci(100);
        
        ELIO_CO_FOR(v, gen) {
            values.push_back(v);
            if (values.size() >= 5) break;
        }
        completed.store(true);
    };
    
    elio::go(consumer);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    REQUIRE(values == std::vector<int>{0, 1, 1, 2, 3});
    
    sched.shutdown();
}

// ============================================================================
// for_each method
// ============================================================================

TEST_CASE("generator for_each", "[generator]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    std::vector<int> values;
    
    auto consumer = [&]() -> task<void> {
        auto gen = simple_producer();
        
        co_await gen.for_each([&](int v) {
            values.push_back(v);
        });
        completed.store(true);
    };
    
    elio::go(consumer);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    REQUIRE(values == std::vector<int>{1, 2, 3});
    
    sched.shutdown();
}

TEST_CASE("generator for_each with early termination", "[generator]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    std::vector<int> values;
    
    auto consumer = [&]() -> task<void> {
        auto gen = fibonacci(100);
        
        co_await gen.for_each([&](int v) -> bool {
            values.push_back(v);
            return values.size() < 5;  // stop after 5 values
        });
        completed.store(true);
    };
    
    elio::go(consumer);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    REQUIRE(values == std::vector<int>{0, 1, 1, 2, 3});
    
    sched.shutdown();
}

// ============================================================================
// Multiple yields in sequence
// ============================================================================

generator<int> multi_yield_producer() {
    co_yield 1;
    co_yield 2;
    co_yield 3;
    co_yield 4;
    co_yield 5;
    co_yield 6;
    co_yield 7;
    co_yield 8;
    co_yield 9;
    co_yield 10;
}

TEST_CASE("generator many yields", "[generator]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    std::vector<int> values;
    
    auto consumer = [&]() -> task<void> {
        auto gen = multi_yield_producer();
        
        while (auto v = co_await gen.next()) {
            values.push_back(*v);
        }
        completed.store(true);
    };
    
    elio::go(consumer);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    REQUIRE(values.size() == 10);
    
    int expected_sum = 55;  // 1+2+...+10
    int actual_sum = 0;
    for (int v : values) actual_sum += v;
    REQUIRE(actual_sum == expected_sum);
    
    sched.shutdown();
}
