#include <catch2/catch_test_macros.hpp>
#include <elio/log/logger.hpp>
#include <elio/log/macros.hpp>
#include <thread>
#include <vector>
#include <sstream>

using namespace elio::log;

TEST_CASE("Logger singleton", "[logger]") {
    auto& logger1 = logger::instance();
    auto& logger2 = logger::instance();
    
    REQUIRE(&logger1 == &logger2);
}

TEST_CASE("Log level filtering", "[logger]") {
    auto& log = logger::instance();
    
    // Set to warning level
    log.set_level(level::warning);
    REQUIRE(log.get_level() == level::warning);
    
    // Debug and info should be filtered (we can't easily test output, but verify no crash)
    ELIO_LOG_INFO("This should be filtered");
    
    // Warning and error should go through
    ELIO_LOG_WARNING("This is a warning");
    ELIO_LOG_ERROR("This is an error");
    
    // Reset to info
    log.set_level(level::info);
    REQUIRE(log.get_level() == level::info);
}

TEST_CASE("Log level conversion", "[logger]") {
    REQUIRE(std::string(level_to_string(level::debug)) == "DEBUG");
    REQUIRE(std::string(level_to_string(level::info)) == "INFO");
    REQUIRE(std::string(level_to_string(level::warning)) == "WARN");
    REQUIRE(std::string(level_to_string(level::error)) == "ERROR");
}

TEST_CASE("Concurrent logging", "[logger]") {
    auto& log = logger::instance();
    log.set_level(level::info);
    
    std::vector<std::thread> threads;
    const int num_threads = 10;
    const int logs_per_thread = 100;
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([i, logs_per_thread]() {
            for (int j = 0; j < logs_per_thread; ++j) {
                ELIO_LOG_INFO("Thread {} log {}", i, j);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // If we get here without crashing, concurrent logging works
    REQUIRE(true);
}

TEST_CASE("Log formatting with various types", "[logger]") {
    auto& log = logger::instance();
    log.set_level(level::info);
    
    // Test various argument types
    ELIO_LOG_INFO("Integer: {}", 42);
    ELIO_LOG_INFO("Float: {}", 3.14);
    ELIO_LOG_INFO("String: {}", "hello");
    ELIO_LOG_INFO("Multiple: {} {} {}", 1, "two", 3.0);
    
    // If we get here without crashing, formatting works
    REQUIRE(true);
}

#ifdef ELIO_DEBUG
TEST_CASE("Debug logging enabled", "[logger]") {
    auto& log = logger::instance();
    log.set_level(level::debug);
    
    // This should compile and run
    ELIO_LOG_DEBUG("Debug message: {}", 123);
    
    REQUIRE(true);
}
#else
TEST_CASE("Debug logging disabled", "[logger]") {
    // In release mode, debug macro should be no-op
    // This test just verifies compilation
    ELIO_LOG_DEBUG("This should be optimized away");
    
    REQUIRE(true);
}
#endif
