#include <elio/runtime/scheduler.hpp>
#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>
#include <iostream>

using namespace elio;

// Simple coroutine that returns a greeting
coro::task<std::string> get_greeting() {
    ELIO_LOG_INFO("Inside get_greeting coroutine");
    co_return "Hello from Elio!";
}

// Main coroutine
coro::task<void> main_task() {
    ELIO_LOG_INFO("Main task started");
    
    // Co-await the greeting
    std::string greeting = co_await get_greeting();
    
    std::cout << greeting << std::endl;
    
    ELIO_LOG_INFO("Main task completed");
    co_return;
}

int main() {
    // Enable debug logging
    log::logger::instance().set_level(log::level::debug);
    
    std::cout << "=== Elio Hello World Example ===" << std::endl;
    
    // Create scheduler with 2 worker threads
    runtime::scheduler sched(2);
    
    // Start the scheduler
    sched.start();
    
    // Spawn the main task (release ownership to scheduler)
    auto t = main_task();
    sched.spawn(t.release());
    
    // Wait for task to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Shutdown the scheduler
    sched.shutdown();
    
    std::cout << "=== Example completed ===" << std::endl;
    
    return 0;
}
