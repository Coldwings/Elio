#include <catch2/catch_test_macros.hpp>
#include <elio/log/logger.hpp>
#include <elio/log/macros.hpp>
#include <atomic>
#include <latch>
#include <string>
#include <thread>
#include <vector>

using namespace elio::log;

namespace {

class scoped_logger_level {
public:
    explicit scoped_logger_level(logger& log) noexcept
        : log_(log), previous_(log.get_level()) {}

    ~scoped_logger_level() noexcept {
        log_.set_level(previous_);
    }

    scoped_logger_level(const scoped_logger_level&) = delete;
    scoped_logger_level& operator=(const scoped_logger_level&) = delete;

private:
    logger& log_;
    level previous_;
};

} // namespace

TEST_CASE("Logger singleton", "[logger]") {
    auto& logger1 = logger::instance();
    auto& logger2 = logger::instance();
    
    REQUIRE(&logger1 == &logger2);
}

TEST_CASE("Log level filtering", "[logger]") {
    auto& log = logger::instance();
    scoped_logger_level restore_level(log);
    
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

TEST_CASE("Logger level changes are scoped", "[logger][regression]") {
    auto& log = logger::instance();
    scoped_logger_level restore_initial_level(log);
    log.set_level(level::warning);

    {
        scoped_logger_level restore_warning_level(log);
        log.set_level(level::debug);
        REQUIRE(log.get_level() == level::debug);
    }

    REQUIRE(log.get_level() == level::warning);
}

TEST_CASE("Log level conversion", "[logger]") {
    REQUIRE(std::string(level_to_string(level::debug)) == "DEBUG");
    REQUIRE(std::string(level_to_string(level::info)) == "INFO");
    REQUIRE(std::string(level_to_string(level::warning)) == "WARN");
    REQUIRE(std::string(level_to_string(level::error)) == "ERROR");
}

TEST_CASE("Concurrent logging", "[logger]") {
    auto& log = logger::instance();
    scoped_logger_level restore_level(log);
    log.set_level(level::info);
    
    std::vector<std::thread> threads;
    const int num_threads = 10;
    const int logs_per_thread = 4;
    std::latch start(num_threads + 1);
    std::atomic<int> completed_logs{0};
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, i]() {
            start.arrive_and_wait();
            for (int j = 0; j < logs_per_thread; ++j) {
                ELIO_LOG_INFO("Thread {} log {}", i, j);
                completed_logs.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.arrive_and_wait();
    
    for (auto& t : threads) {
        t.join();
    }
    
    REQUIRE(completed_logs.load(std::memory_order_relaxed) ==
            num_threads * logs_per_thread);
}

TEST_CASE("Log formatting with various types", "[logger]") {
    auto& log = logger::instance();
    scoped_logger_level restore_level(log);
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
    scoped_logger_level restore_level(log);
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
