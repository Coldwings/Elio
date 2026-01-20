#include <elio/elio.hpp>
#include <iostream>

using namespace elio;

// Innermost coroutine (level 3)
coro::task<int> level3_compute() {
    ELIO_LOG_INFO("Level 3: Computing base value");
    size_t depth = coro::get_stack_depth();
    std::cout << "  [Level 3] Virtual stack depth: " << depth << std::endl;
    co_return 10;
}

// Middle coroutine (level 2)
coro::task<int> level2_multiply(int multiplier) {
    ELIO_LOG_INFO("Level 2: Awaiting level 3");
    size_t depth = coro::get_stack_depth();
    std::cout << "  [Level 2] Virtual stack depth: " << depth << std::endl;
    
    int base = co_await level3_compute();
    int result = base * multiplier;
    
    std::cout << "  [Level 2] Result: " << base << " * " << multiplier << " = " << result << std::endl;
    co_return result;
}

// Outer coroutine (level 1)
coro::task<int> level1_orchestrate() {
    ELIO_LOG_INFO("Level 1: Starting orchestration");
    size_t depth = coro::get_stack_depth();
    std::cout << "  [Level 1] Virtual stack depth: " << depth << std::endl;
    
    int result1 = co_await level2_multiply(2);
    int result2 = co_await level2_multiply(3);
    
    int total = result1 + result2;
    std::cout << "  [Level 1] Total: " << result1 << " + " << result2 << " = " << total << std::endl;
    
    ELIO_LOG_INFO("Level 1: Orchestration complete");
    co_return total;
}

coro::task<int> async_main() {
    // Enable debug logging
    log::logger::instance().set_level(log::level::debug);
    
    std::cout << "=== Elio Chained Coroutines Example ===" << std::endl;
    std::cout << "Demonstrating virtual stack tracking across 3 levels of coroutines" << std::endl;
    std::cout << std::endl;
    
    int result = co_await level1_orchestrate();
    
    std::cout << std::endl;
    std::cout << "=== Example completed ===" << std::endl;
    std::cout << "Virtual stack automatically tracked call chain:" << std::endl;
    std::cout << "  level1_orchestrate -> level2_multiply -> level3_compute" << std::endl;
    std::cout << "Final result: " << result << std::endl;
    
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
