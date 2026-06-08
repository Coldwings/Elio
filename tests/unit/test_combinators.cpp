#define ELIO_EXPERIMENTAL
#include <catch2/catch_test_macros.hpp>
#include <elio/elio.hpp>
#include <elio/coro/when_all.hpp>
#include <elio/coro/when_any.hpp>
#include <elio/time/timer.hpp>
#include <atomic>
#include <stdexcept>
#include "../test_main.cpp"

using namespace elio;
using namespace elio::coro;
using namespace elio::test;

// --- when_all tests ---

TEST_CASE("when_all completes all tasks", "[sync][combinators]") {
    auto test = []() -> task<void> {
        auto [a, b, c] = co_await when_all(
            []() -> task<int> {
                co_await time::sleep_for(std::chrono::milliseconds(1));
                co_return 10;
            },
            []() -> task<int> {
                co_await time::sleep_for(std::chrono::milliseconds(1));
                co_return 20;
            },
            []() -> task<int> {
                co_await time::sleep_for(std::chrono::milliseconds(1));
                co_return 30;
            }
        );
        REQUIRE(a == 10);
        REQUIRE(b == 20);
        REQUIRE(c == 30);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("when_all with void tasks", "[sync][combinators]") {
    std::atomic<int> counter{0};

    auto test = [&]() -> task<void> {
        co_await when_all(
            [&]() -> task<void> {
                co_await time::sleep_for(std::chrono::milliseconds(1));
                counter.fetch_add(1, std::memory_order_relaxed);
            },
            [&]() -> task<void> {
                co_await time::sleep_for(std::chrono::milliseconds(1));
                counter.fetch_add(1, std::memory_order_relaxed);
            },
            [&]() -> task<void> {
                co_await time::sleep_for(std::chrono::milliseconds(1));
                counter.fetch_add(1, std::memory_order_relaxed);
            }
        );
        REQUIRE(counter.load() == 3);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("when_all propagates first exception", "[sync][combinators]") {
    auto test = []() -> task<void> {
        bool caught = false;
        try {
            co_await when_all(
                []() -> task<int> {
                    co_await time::sleep_for(std::chrono::milliseconds(1));
                    co_return 1;
                },
                []() -> task<int> {
                    co_await time::sleep_for(std::chrono::milliseconds(1));
                    throw std::runtime_error("test error");
                    co_return 0;
                },
                []() -> task<int> {
                    co_await time::sleep_for(std::chrono::milliseconds(1));
                    co_return 3;
                }
            );
        } catch (const std::runtime_error& e) {
            caught = true;
            REQUIRE(std::string(e.what()) == "test error");
        }
        REQUIRE(caught);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("when_all single task", "[sync][combinators]") {
    auto test = []() -> task<void> {
        auto result = co_await when_all(
            []() -> task<int> { co_return 42; }
        );
        REQUIRE(result == 42);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

// --- when_any tests ---

TEST_CASE("when_any returns first completer", "[sync][combinators]") {
    auto test = []() -> task<void> {
        auto [idx, result] = co_await when_any(
            []() -> task<int> {
                co_await time::sleep_for(std::chrono::milliseconds(1));
                co_return 1;
            },
            []() -> task<int> {
                co_await time::sleep_for(std::chrono::milliseconds(500));
                co_return 2;
            }
        );
        REQUIRE(idx == 0);
        REQUIRE(std::get<0>(result) == 1);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("when_any second finishes first", "[sync][combinators]") {
    auto test = []() -> task<void> {
        auto [idx, result] = co_await when_any(
            []() -> task<int> {
                co_await time::sleep_for(std::chrono::milliseconds(500));
                co_return 1;
            },
            []() -> task<int> {
                co_await time::sleep_for(std::chrono::milliseconds(1));
                co_return 2;
            }
        );
        REQUIRE(idx == 1);
        REQUIRE(std::get<1>(result) == 2);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("when_any propagates exception from winner", "[sync][combinators]") {
    auto test = []() -> task<void> {
        bool caught = false;
        try {
            co_await when_any(
                []() -> task<int> {
                    co_await time::sleep_for(std::chrono::milliseconds(1));
                    throw std::runtime_error("test error");
                    co_return 0;
                },
                []() -> task<int> {
                    co_await time::sleep_for(std::chrono::milliseconds(500));
                    co_return 2;
                }
            );
        } catch (const std::runtime_error& e) {
            caught = true;
            REQUIRE(std::string(e.what()) == "test error");
        }
        REQUIRE(caught);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}
