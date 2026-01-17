#include <elio/runtime/scheduler.hpp>
#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>
#include <iostream>
#include <stdexcept>

using namespace elio;

// Coroutine that may throw an exception
coro::task<int> risky_operation(int value) {
    ELIO_LOG_INFO("Risky operation with value: {}", value);
    
    if (value < 0) {
        ELIO_LOG_WARNING("Negative value detected, throwing exception");
        throw std::runtime_error("Cannot process negative value: " + std::to_string(value));
    }
    
    co_return value * 2;
}

// Middle layer that propagates exceptions
coro::task<int> process_batch(int a, int b) {
    ELIO_LOG_INFO("Processing batch: {}, {}", a, b);
    
    int result1 = co_await risky_operation(a);
    int result2 = co_await risky_operation(b);
    
    co_return result1 + result2;
}

// Top-level coroutine with exception handling
coro::task<void> safe_executor() {
    std::cout << "Executing safe operations..." << std::endl;
    
    try {
        int result = co_await process_batch(5, 10);
        std::cout << "  ✓ Success: Result = " << result << std::endl;
    } catch (const std::exception& e) {
        std::cout << "  ✗ Should not happen: " << e.what() << std::endl;
    }
    
    std::cout << std::endl;
    std::cout << "Executing operation that will throw..." << std::endl;
    
    try {
        int result = co_await process_batch(5, -10);  // Will throw
        std::cout << "  ✗ Should not reach here: " << result << std::endl;
    } catch (const std::exception& e) {
        std::cout << "  ✓ Exception caught: " << e.what() << std::endl;
    }
    
    std::cout << std::endl;
    std::cout << "Continuing after exception..." << std::endl;
    
    try {
        int result = co_await process_batch(7, 8);
        std::cout << "  ✓ Success after recovery: Result = " << result << std::endl;
    } catch (const std::exception& e) {
        std::cout << "  ✗ Unexpected error: " << e.what() << std::endl;
    }
    
    co_return;
}

// Deep exception propagation test
coro::task<int> level4() {
    throw std::runtime_error("Exception from level 4");
    co_return 0;
}

coro::task<int> level3() {
    co_return co_await level4();
}

coro::task<int> level2() {
    co_return co_await level3();
}

coro::task<void> level1() {
    std::cout << "Testing deep exception propagation (4 levels)..." << std::endl;
    
    try {
        int result = co_await level2();
        std::cout << "  ✗ Should not reach: " << result << std::endl;
    } catch (const std::runtime_error& e) {
        std::cout << "  ✓ Exception propagated through 4 levels: " << e.what() << std::endl;
    }
    
    co_return;
}

int main() {
    // Enable info logging
    log::logger::instance().set_level(log::level::info);
    
    std::cout << "=== Elio Exception Handling Example ===" << std::endl;
    std::cout << "Demonstrating exception propagation through virtual stack" << std::endl;
    std::cout << std::endl;
    
    // Create scheduler
    runtime::scheduler sched(2);
    sched.start();
    
    // Run safe executor
    auto t1 = safe_executor();
    sched.spawn(t1.release());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    std::cout << std::endl;
    
    // Run deep propagation test
    auto t2 = level1();
    sched.spawn(t2.release());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Shutdown
    sched.shutdown();
    
    std::cout << std::endl;
    std::cout << "=== Example completed ===" << std::endl;
    std::cout << "All exceptions were properly propagated and caught!" << std::endl;
    
    return 0;
}
