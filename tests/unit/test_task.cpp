#include <catch2/catch_test_macros.hpp>
#include <elio/coro/task.hpp>
#include <elio/coro/frame.hpp>
#include <string>

using namespace elio::coro;

// Helper: Simple coroutine that returns a value
task<int> simple_return_value() {
    co_return 42;
}

// Helper: Simple void coroutine
task<void> simple_void() {
    co_return;
}

// Helper: Coroutine that throws
task<int> throwing_coroutine() {
    throw std::runtime_error("test error");
    co_return 0;  // Unreachable
}

// Helper: Nested coroutines
task<int> nested_inner() {
    co_return 10;
}

task<int> nested_outer() {
    int value = co_await nested_inner();
    co_return value * 2;
}

TEST_CASE("task construction and destruction", "[task]") {
    {
        auto t = simple_return_value();
        REQUIRE(t.handle() != nullptr);
    }
    // Task should destroy handle in destructor
}

TEST_CASE("task move semantics", "[task]") {
    auto t1 = simple_return_value();
    auto h1 = t1.handle();
    REQUIRE(h1 != nullptr);
    
    auto t2 = std::move(t1);
    REQUIRE(t1.handle() == nullptr);  // Moved-from
    REQUIRE(t2.handle() == h1);       // Moved-to
}

TEST_CASE("task<int> co_return value", "[task]") {
    auto t = simple_return_value();
    
    // Start the coroutine
    t.handle().resume();
    
    // The promise should have the value
    REQUIRE(t.handle().promise().value_.has_value());
    REQUIRE(t.handle().promise().value_.value() == 42);
}

TEST_CASE("task<void> co_return void", "[task]") {
    auto t = simple_void();
    
    // Start the coroutine
    t.handle().resume();
    
    // Should complete without error
    REQUIRE(t.handle().done());
}

TEST_CASE("task stores exception", "[task]") {
    auto t = throwing_coroutine();
    
    // Start the coroutine
    t.handle().resume();
    
    // The promise should have an exception
    REQUIRE(t.handle().promise().exception() != nullptr);
}

TEST_CASE("task co_await basic", "[task]") {
    auto outer = []() -> task<int> {
        auto t = simple_return_value();
        int result = co_await t;
        REQUIRE(result == 42);
        co_return result + 1;
    };
    
    auto t = outer();
    t.handle().resume();
    
    // The outer task should have result 43
    REQUIRE(t.handle().promise().value_.value() == 43);
}

TEST_CASE("task nested co_await", "[task]") {
    auto t = nested_outer();
    t.handle().resume();
    
    // The outer coroutine should return 20 (10 * 2)
    REQUIRE(t.handle().promise().value_.value() == 20);
}

TEST_CASE("task exception propagation via co_await", "[task]") {
    auto outer = []() -> task<void> {
        try {
            auto t = throwing_coroutine();
            co_await t;
            FAIL("Should have thrown");
        } catch (const std::runtime_error& e) {
            REQUIRE(std::string(e.what()) == "test error");
        }
    };
    
    auto t = outer();
    t.handle().resume();
    
    // Should complete without unhandled exception
    REQUIRE(t.handle().done());
}

TEST_CASE("task virtual stack integration", "[task]") {
    auto inner = []() -> task<int> {
        // Inside inner coroutine, virtual stack should be at least 1 deep
        size_t depth = get_stack_depth();
        REQUIRE(depth >= 1);
        co_return 100;
    };
    
    auto outer = [&]() -> task<int> {
        size_t outer_depth = get_stack_depth();
        int result = co_await inner();
        size_t inner_depth = get_stack_depth();
        
        // After co_await, we should be back to outer depth
        REQUIRE(inner_depth == outer_depth);
        co_return result;
    };
    
    auto t = outer();
    t.handle().resume();
}

TEST_CASE("task multiple levels", "[task]") {
    auto level3 = []() -> task<int> { co_return 1; };
    auto level2 = [&]() -> task<int> {
        int v = co_await level3();
        co_return v + 1;
    };
    auto level1 = [&]() -> task<int> {
        int v = co_await level2();
        co_return v + 1;
    };
    
    auto t = level1();
    t.handle().resume();
    
    REQUIRE(t.handle().promise().value_.value() == 3);
}

TEST_CASE("task<void> exception propagation", "[task]") {
    auto thrower = []() -> task<void> {
        throw std::runtime_error("void error");
        co_return;
    };
    
    auto catcher = [&]() -> task<void> {
        try {
            co_await thrower();
            FAIL("Should have thrown");
        } catch (const std::runtime_error& e) {
            REQUIRE(std::string(e.what()) == "void error");
        }
    };
    
    auto t = catcher();
    t.handle().resume();
    REQUIRE(t.handle().done());
}
