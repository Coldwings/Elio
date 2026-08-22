#include <catch2/catch_test_macros.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/coro/task.hpp>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include "../test_main.cpp"  // For scaled timeouts

using namespace elio::runtime;
using namespace elio::coro;
using namespace elio::test;

namespace {

enum class exception_outcome {
    pending,
    returned_without_exception,
    caught_expected,
};

struct exception_observation {
    exception_outcome outcome = exception_outcome::pending;
    std::string message;
};

}  // namespace

TEST_CASE("Exception propagation through single level", "[exception]") {
    scheduler sched(2);
    sched.start();
    
    exception_observation observation;
    
    auto thrower = []() -> task<int> {
        throw std::runtime_error("test error");
        co_return 0;
    };
    
    auto catcher = [&]() -> task<void> {
        try {
            int value = co_await thrower();
            (void)value;
            observation.outcome = exception_outcome::returned_without_exception;
        } catch (const std::runtime_error& e) {
            observation.message = e.what();
            observation.outcome = exception_outcome::caught_expected;
        }
        co_return;
    };
    
    auto handle = sched.go_joinable(catcher);
    const bool drained = sched.shutdown(scaled_sec(10));
    const auto observed_outcome = observation.outcome;
    const std::string observed_message = observation.message;

    CAPTURE(drained, static_cast<int>(observed_outcome), observed_message);
    REQUIRE(drained);
    REQUIRE_NOTHROW(handle.await_resume());
    REQUIRE(observed_outcome == exception_outcome::caught_expected);
    REQUIRE(observed_message == "test error");
}

TEST_CASE("Exception propagation through multiple levels", "[exception]") {
    scheduler sched(2);
    sched.start();
    
    exception_observation observation;
    std::atomic<int> level_passed{0};
    
    auto thrower = []() -> task<int> {
        throw std::runtime_error("deep error");
        co_return 0;
    };
    
    auto level3 = [&]() -> task<int> {
        level_passed.fetch_add(1);
        int value = co_await thrower();
        co_return value;
    };
    
    auto level2 = [&]() -> task<int> {
        level_passed.fetch_add(1);
        int value = co_await level3();
        co_return value;
    };
    
    auto level1 = [&]() -> task<void> {
        level_passed.fetch_add(1);
        try {
            int value = co_await level2();
            (void)value;
            observation.outcome = exception_outcome::returned_without_exception;
        } catch (const std::runtime_error& e) {
            observation.message = e.what();
            observation.outcome = exception_outcome::caught_expected;
        }
        co_return;
    };
    
    auto handle = sched.go_joinable(level1);
    const bool drained = sched.shutdown(scaled_sec(10));
    const auto observed_outcome = observation.outcome;
    const std::string observed_message = observation.message;
    const int observed_levels = level_passed.load();

    CAPTURE(drained, static_cast<int>(observed_outcome), observed_message,
            observed_levels);
    REQUIRE(drained);
    REQUIRE_NOTHROW(handle.await_resume());
    REQUIRE(observed_outcome == exception_outcome::caught_expected);
    REQUIRE(observed_message == "deep error");
    REQUIRE(observed_levels == 3);  // All levels should execute
}

TEST_CASE("Exception propagation with void tasks", "[exception]") {
    scheduler sched(2);
    sched.start();
    
    exception_observation observation;
    
    auto thrower = []() -> task<void> {
        throw std::runtime_error("void error");
        co_return;
    };
    
    auto catcher = [&]() -> task<void> {
        try {
            co_await thrower();
            observation.outcome = exception_outcome::returned_without_exception;
        } catch (const std::runtime_error& e) {
            observation.message = e.what();
            observation.outcome = exception_outcome::caught_expected;
        }
        co_return;
    };
    
    auto handle = sched.go_joinable(catcher);
    const bool drained = sched.shutdown(scaled_sec(10));
    const auto observed_outcome = observation.outcome;
    const std::string observed_message = observation.message;

    CAPTURE(drained, static_cast<int>(observed_outcome), observed_message);
    REQUIRE(drained);
    REQUIRE_NOTHROW(handle.await_resume());
    REQUIRE(observed_outcome == exception_outcome::caught_expected);
    REQUIRE(observed_message == "void error");
}

TEST_CASE("Multiple exceptions in different coroutines", "[exception]") {
    scheduler sched(4);
    sched.start();
    
    std::atomic<int> caught_count{0};
    
    auto thrower = [](int id) -> task<void> {
        throw std::runtime_error("error " + std::to_string(id));
        co_return;
    };
    
    auto catcher = [&](int id) -> task<void> {
        try {
            co_await thrower(id);
        } catch (const std::runtime_error& e) {
            caught_count.fetch_add(1);
        }
        co_return;
    };
    
    const int num_tasks = 10;
    for (int i = 0; i < num_tasks; ++i) {
        sched.go([&, i]() { return catcher(i); });
    }
    
    // Active wait for completion with timeout
    auto start = std::chrono::steady_clock::now();
    while (caught_count.load() < num_tasks) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > scaled_sec(10)) {
            break;
        }
    }
    
    REQUIRE(caught_count.load() == num_tasks);
    
    sched.shutdown();
}

TEST_CASE("Exception in middle of chain", "[exception]") {
    scheduler sched(2);
    sched.start();
    
    exception_observation observation;
    std::atomic<int> level1_executed{0};
    std::atomic<int> level3_executed{0};
    
    auto level3 = [&]() -> task<int> {
        level3_executed.fetch_add(1);
        co_return 100;
    };
    
    auto level2 = [&]() -> task<int> {
        throw std::runtime_error("middle error");
        int value = co_await level3();  // Should not execute
        co_return value;
    };
    
    auto level1 = [&]() -> task<void> {
        level1_executed.fetch_add(1);
        try {
            int value = co_await level2();
            (void)value;
            observation.outcome = exception_outcome::returned_without_exception;
        } catch (const std::runtime_error& e) {
            observation.message = e.what();
            observation.outcome = exception_outcome::caught_expected;
        }
        co_return;
    };
    
    auto handle = sched.go_joinable(level1);
    const bool drained = sched.shutdown(scaled_sec(10));
    const auto observed_outcome = observation.outcome;
    const std::string observed_message = observation.message;
    const int observed_level1 = level1_executed.load();
    const int observed_level3 = level3_executed.load();

    CAPTURE(drained, static_cast<int>(observed_outcome), observed_message,
            observed_level1, observed_level3);
    REQUIRE(drained);
    REQUIRE_NOTHROW(handle.await_resume());
    REQUIRE(observed_outcome == exception_outcome::caught_expected);
    REQUIRE(observed_message == "middle error");
    REQUIRE(observed_level1 == 1);
    REQUIRE(observed_level3 == 0);  // Should not execute
}

TEST_CASE("Exception with custom exception type", "[exception]") {
    scheduler sched(2);
    sched.start();
    
    exception_observation observation;
    
    struct custom_exception : std::exception {
        const char* what() const noexcept override {
            return "custom error";
        }
    };
    
    auto thrower = []() -> task<void> {
        throw custom_exception();
        co_return;
    };
    
    auto catcher = [&]() -> task<void> {
        try {
            co_await thrower();
            observation.outcome = exception_outcome::returned_without_exception;
        } catch (const custom_exception& e) {
            observation.message = e.what();
            observation.outcome = exception_outcome::caught_expected;
        }
        co_return;
    };
    
    auto handle = sched.go_joinable(catcher);
    const bool drained = sched.shutdown(scaled_sec(10));
    const auto observed_outcome = observation.outcome;
    const std::string observed_message = observation.message;

    CAPTURE(drained, static_cast<int>(observed_outcome), observed_message);
    REQUIRE(drained);
    REQUIRE_NOTHROW(handle.await_resume());
    REQUIRE(observed_outcome == exception_outcome::caught_expected);
    REQUIRE(observed_message == "custom error");
}

TEST_CASE("Uncaught exception in coroutine", "[exception]") {
    scheduler sched(2);
    sched.start();
    
    auto thrower = []() -> task<void> {
        throw std::runtime_error("uncaught");
        co_return;
    };
    
    sched.go(thrower);
    
    // Should not crash the scheduler
    std::this_thread::sleep_for(scaled_ms(200));
    
    // Scheduler should still be running
    REQUIRE(sched.is_running());
    
    sched.shutdown();
}

TEST_CASE("Exception propagation preserves exception message", "[exception]") {
    scheduler sched(2);
    sched.start();
    
    exception_observation observation;
    const std::string expected_msg = "detailed error message with context";
    
    auto thrower = [&]() -> task<int> {
        throw std::runtime_error(expected_msg);
        co_return 0;
    };
    
    auto level2 = [&]() -> task<int> {
        int value = co_await thrower();
        co_return value;
    };
    
    auto level1 = [&]() -> task<void> {
        try {
            int value = co_await level2();
            (void)value;
            observation.outcome = exception_outcome::returned_without_exception;
        } catch (const std::runtime_error& e) {
            observation.message = e.what();
            observation.outcome = exception_outcome::caught_expected;
        }
        co_return;
    };
    
    auto handle = sched.go_joinable(level1);
    const bool drained = sched.shutdown(scaled_sec(10));
    const auto observed_outcome = observation.outcome;
    const std::string observed_message = observation.message;

    CAPTURE(drained, static_cast<int>(observed_outcome), observed_message);
    REQUIRE(drained);
    REQUIRE_NOTHROW(handle.await_resume());
    REQUIRE(observed_outcome == exception_outcome::caught_expected);
    REQUIRE(observed_message == expected_msg);
}
