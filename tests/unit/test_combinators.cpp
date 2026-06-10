#include <catch2/catch_test_macros.hpp>
#include <elio/elio.hpp>
#include <elio/coro/when_all.hpp>
#include <elio/coro/when_any.hpp>
#include <elio/coro/with_timeout.hpp>
#include <elio/time/timer.hpp>
#include <atomic>
#include <stdexcept>
#include <string>
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
        auto [result] = co_await when_all(
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
        auto [idx, value] = co_await when_any(
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
        REQUIRE(value == 1);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("when_any second finishes first", "[sync][combinators]") {
    auto test = []() -> task<void> {
        auto [idx, value] = co_await when_any(
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
        REQUIRE(value == 2);
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

TEST_CASE("when_any with cancel_token propagation", "[sync][combinators]") {
    std::atomic<bool> was_cancelled{false};

    auto test = [&]() -> task<void> {
        auto [idx, value] = co_await when_any(
            []() -> task<int> {
                co_await time::sleep_for(std::chrono::milliseconds(1));
                co_return 42;
            },
            [&](coro::cancel_token tok) -> task<int> {
                auto r = co_await time::sleep_for(
                    std::chrono::milliseconds(500), tok);
                if (r == coro::cancel_result::cancelled) {
                    was_cancelled.store(true, std::memory_order_relaxed);
                }
                co_return -1;
            }
        );
        REQUIRE(idx == 0);
        REQUIRE(value == 42);
        co_await time::sleep_for(std::chrono::milliseconds(50));
        REQUIRE(was_cancelled.load(std::memory_order_relaxed));
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("when_any with void tasks", "[sync][combinators]") {
    std::atomic<int> winner{-1};

    auto test = [&]() -> task<void> {
        auto [idx, mono] = co_await when_any(
            [&]() -> task<void> {
                co_await time::sleep_for(std::chrono::milliseconds(1));
                winner.store(0, std::memory_order_relaxed);
            },
            [&]() -> task<void> {
                co_await time::sleep_for(std::chrono::milliseconds(500));
                winner.store(1, std::memory_order_relaxed);
            }
        );
        REQUIRE(idx == 0);
        static_assert(std::is_same_v<decltype(mono), std::monostate>);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("when_any single task", "[sync][combinators]") {
    auto test = []() -> task<void> {
        auto [idx, value] = co_await when_any(
            []() -> task<int> { co_return 99; }
        );
        REQUIRE(idx == 0);
        REQUIRE(value == 99);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("when_any with heterogeneous types", "[sync][combinators]") {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
    auto test = []() -> task<void> {
        auto [idx, result] = co_await when_any(
            []() -> task<int> {
                co_await time::sleep_for(std::chrono::milliseconds(1));
                co_return 42;
            },
            []() -> task<std::string> {
                co_await time::sleep_for(std::chrono::milliseconds(500));
                co_return std::string("hello");
            }
        );
        REQUIRE(idx == 0);
        REQUIRE(std::get<0>(result) == 42);
    };
#pragma GCC diagnostic pop

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("when_any loser exception is silent", "[sync][combinators]") {
    auto test = []() -> task<void> {
        auto [idx, value] = co_await when_any(
            []() -> task<int> {
                co_await time::sleep_for(std::chrono::milliseconds(1));
                co_return 42;
            },
            []() -> task<int> {
                co_await time::sleep_for(std::chrono::milliseconds(50));
                throw std::runtime_error("loser exception");
                co_return 0;
            }
        );
        REQUIRE(idx == 0);
        REQUIRE(value == 42);
        co_await time::sleep_for(std::chrono::milliseconds(200));
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

// --- with_timeout tests ---

TEST_CASE("with_timeout task completes before timeout", "[sync][combinators]") {
    auto test = []() -> task<void> {
        auto result = co_await with_timeout(
            std::chrono::milliseconds(500),
            []() -> task<int> {
                co_await time::sleep_for(std::chrono::milliseconds(1));
                co_return 42;
            }
        );
        REQUIRE(static_cast<bool>(result));
        REQUIRE(!result.timed_out);
        REQUIRE(*result == 42);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("with_timeout task exceeds timeout", "[sync][combinators]") {
    auto test = []() -> task<void> {
        auto result = co_await with_timeout(
            std::chrono::milliseconds(1),
            [](coro::cancel_token tok) -> task<int> {
                co_await time::sleep_for(std::chrono::milliseconds(500), tok);
                co_return 42;
            }
        );
        REQUIRE(!static_cast<bool>(result));
        REQUIRE(result.timed_out);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("with_timeout with cancel_token propagation", "[sync][combinators]") {
    std::atomic<bool> was_cancelled{false};

    auto test = [&]() -> task<void> {
        auto result = co_await with_timeout(
            std::chrono::milliseconds(1),
            [&](coro::cancel_token tok) -> task<int> {
                auto r = co_await time::sleep_for(
                    std::chrono::milliseconds(500), tok);
                if (r == coro::cancel_result::cancelled) {
                    was_cancelled.store(true, std::memory_order_relaxed);
                }
                co_return -1;
            }
        );
        REQUIRE(result.timed_out);
        co_await time::sleep_for(std::chrono::milliseconds(50));
        REQUIRE(was_cancelled.load(std::memory_order_relaxed));
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("with_timeout with void task", "[sync][combinators]") {
    std::atomic<bool> completed{false};

    auto test = [&]() -> task<void> {
        auto result = co_await with_timeout(
            std::chrono::milliseconds(500),
            [&]() -> task<void> {
                co_await time::sleep_for(std::chrono::milliseconds(1));
                completed.store(true, std::memory_order_relaxed);
            }
        );
        REQUIRE(static_cast<bool>(result));
        REQUIRE(!result.timed_out);
        REQUIRE(completed.load(std::memory_order_relaxed));
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("with_timeout with zero duration", "[sync][combinators]") {
    auto test = []() -> task<void> {
        auto result = co_await with_timeout(
            std::chrono::milliseconds(0),
            [](coro::cancel_token tok) -> task<int> {
                co_await time::sleep_for(std::chrono::milliseconds(100), tok);
                co_return 42;
            }
        );
        REQUIRE(result.timed_out);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("with_timeout task throws exception", "[sync][combinators]") {
    auto test = []() -> task<void> {
        bool caught = false;
        try {
            co_await with_timeout(
                std::chrono::milliseconds(500),
                []() -> task<int> {
                    co_await time::sleep_for(std::chrono::milliseconds(1));
                    throw std::runtime_error("task error");
                    co_return 0;
                }
            );
        } catch (const std::runtime_error& e) {
            caught = true;
            REQUIRE(std::string(e.what()) == "task error");
        }
        REQUIRE(caught);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}
