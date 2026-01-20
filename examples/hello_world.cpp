#include <elio/elio.hpp>
#include <iostream>

using namespace elio;

// Simple coroutine that returns a greeting
coro::task<std::string> get_greeting() {
    ELIO_LOG_INFO("Inside get_greeting coroutine");
    co_return "Hello from Elio!";
}

// Async main coroutine with command line arguments
coro::task<int> async_main(int argc, char* argv[]) {
    // Enable debug logging
    log::logger::instance().set_level(log::level::debug);
    
    std::cout << "=== Elio Hello World Example ===" << std::endl;
    
    if (argc > 1) {
        std::cout << "Arguments: ";
        for (int i = 1; i < argc; ++i) {
            std::cout << argv[i] << " ";
        }
        std::cout << std::endl;
    }
    
    ELIO_LOG_INFO("Main task started");
    
    // Co-await the greeting
    std::string greeting = co_await get_greeting();
    
    std::cout << greeting << std::endl;
    
    ELIO_LOG_INFO("Main task completed");
    std::cout << "=== Example completed ===" << std::endl;
    
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
