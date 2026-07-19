#include <catch2/catch_test_macros.hpp>
#include <elio/elio.hpp>
#include <elio/coro/when_all.hpp>
#include <elio/coro/when_any.hpp>
#include <elio/coro/with_timeout.hpp>
#include <elio/time/timer.hpp>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include "../test_main.cpp"

using namespace elio;
using namespace elio::coro;
using namespace elio::test;

namespace {

bool wait_for_flag(const std::atomic<bool>& value) {
    const auto deadline = std::chrono::steady_clock::now() + scaled_ms(2000);
    while (!value.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return value.load(std::memory_order_acquire);
}

struct throwing_move_callable {
    throwing_move_callable() = default;
    throwing_move_callable(const throwing_move_callable&) = delete;
    throwing_move_callable& operator=(const throwing_move_callable&) = delete;
    throwing_move_callable(throwing_move_callable&&) noexcept(false) {}
    throwing_move_callable& operator=(throwing_move_callable&&) = delete;

    task<int> operator()() {
        co_return 1;
    }
};

struct throwing_result {
    explicit throwing_result(int value) : value(value) {}
    throwing_result() = delete;
    throwing_result(const throwing_result&) = delete;
    throwing_result& operator=(const throwing_result&) = delete;

    throwing_result(throwing_result&& other) {
        if (throw_on_move.exchange(false, std::memory_order_relaxed)) {
            throw std::runtime_error("result transfer failed");
        }
        value = other.value;
    }

    throwing_result& operator=(throwing_result&&) = delete;

    int value;
    static inline std::atomic<bool> throw_on_move{false};
};

struct non_default_result {
    explicit non_default_result(int input) : value(input) {}
    non_default_result() = delete;
    non_default_result(const non_default_result&) = delete;
    non_default_result& operator=(const non_default_result&) = delete;
    non_default_result(non_default_result&&) noexcept = default;
    non_default_result& operator=(non_default_result&&) noexcept = default;

    int value;
};

struct launch_blocker {
    std::atomic<bool> block_moves{false};
    std::atomic<bool> move_started{false};
    std::atomic<bool> allow_move{false};
};

struct blocking_move_callable {
    launch_blocker* blocker{};

    explicit blocking_move_callable(launch_blocker& b) : blocker(&b) {}
    blocking_move_callable(const blocking_move_callable&) = delete;
    blocking_move_callable& operator=(const blocking_move_callable&) = delete;

    blocking_move_callable(blocking_move_callable&& other) noexcept
        : blocker(other.blocker) {
        if (!blocker || !blocker->block_moves.load(std::memory_order_acquire)) {
            return;
        }
        blocker->move_started.store(true, std::memory_order_release);
        while (!blocker->allow_move.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    blocking_move_callable& operator=(blocking_move_callable&&) = delete;

    task<int> operator()() {
        co_return 2;
    }
};

// Launch-failure tests pin their launcher to the last worker so this deliberate
// synchronous block cannot stall the already-submitted round-robin children.
struct launch_throw_control {
    std::atomic<bool> block_on_move{false};
    std::atomic<bool> move_started{false};
    std::atomic<bool> allow_move{false};
    std::atomic<bool> throw_on_move{false};
};

struct launch_throwing_move_callable {
    launch_throw_control* control{};

    explicit launch_throwing_move_callable(launch_throw_control& c)
        : control(&c) {}
    launch_throwing_move_callable(const launch_throwing_move_callable&) = delete;
    launch_throwing_move_callable& operator=(
        const launch_throwing_move_callable&) = delete;

    launch_throwing_move_callable(launch_throwing_move_callable&& other)
        noexcept(false)
        : control(other.control) {
        if (control &&
            control->block_on_move.load(std::memory_order_acquire)) {
            control->move_started.store(true, std::memory_order_release);
            control->move_started.notify_all();
            while (!control->allow_move.load(std::memory_order_acquire)) {
                control->allow_move.wait(false, std::memory_order_acquire);
            }
        }
        if (control && control->throw_on_move.load(std::memory_order_acquire)) {
            throw std::runtime_error("launch move failed");
        }
        other.control = nullptr;
    }

    launch_throwing_move_callable& operator=(
        launch_throwing_move_callable&&) = delete;

    task<int> operator()() {
        co_return 3;
    }
};

} // namespace

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

TEST_CASE("when_all waits for a token-ignoring sibling after failure",
          "[sync][combinators][structured][lifetime]") {
    sync::event sibling_started;
    sync::event release_sibling;
    std::atomic<bool> failure_thrown{false};
    std::atomic<bool> combinator_returned{false};

    runtime::scheduler sched(2);
    sched.start();
    auto owner = sched.go_joinable([&]() -> task<void> {
        bool caught = false;
        try {
            (void)co_await when_all(
                [&]() -> task<int> {
                    co_await sibling_started.wait();
                    failure_thrown.store(true, std::memory_order_release);
                    throw std::runtime_error("primary failure");
                    co_return 0;
                },
                [&]() -> task<int> {
                    sibling_started.set();
                    co_await release_sibling.wait();
                    co_return 2;
                });
        } catch (const std::runtime_error& error) {
            caught = true;
            REQUIRE(std::string_view(error.what()) == "primary failure");
        }
        REQUIRE(caught);
        combinator_returned.store(true, std::memory_order_release);
    });

    REQUIRE(wait_for_flag(failure_thrown));
    REQUIRE_FALSE(combinator_returned.load(std::memory_order_acquire));
    REQUIRE_FALSE(owner.is_ready());
    release_sibling.set();
    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(combinator_returned.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("when_all reports secondary exceptions after drain",
          "[sync][combinators][structured][exception]") {
    sync::event both_started;
    std::atomic<int> started{0};
    std::atomic<int> reported{0};
    std::string secondary_message;

    runtime::scheduler sched(2);
    sched.set_unhandled_exception_handler(
        [&](std::exception_ptr exception) {
            try {
                std::rethrow_exception(exception);
            } catch (const std::runtime_error& error) {
                secondary_message = error.what();
                reported.fetch_add(1, std::memory_order_release);
            }
        });
    sched.start();

    auto make_failure = [&](std::string_view message) {
        return [&, message]() -> task<int> {
            if (started.fetch_add(1, std::memory_order_acq_rel) + 1 == 2) {
                both_started.set();
            }
            co_await both_started.wait();
            throw std::runtime_error(std::string(message));
            co_return 0;
        };
    };

    auto owner = sched.go_joinable([&]() -> task<void> {
        std::string primary_message;
        try {
            (void)co_await when_all(
                make_failure("first branch failure"),
                make_failure("second branch failure"));
        } catch (const std::runtime_error& error) {
            primary_message = error.what();
        }

        REQUIRE(reported.load(std::memory_order_acquire) == 1);
        REQUIRE(primary_message != secondary_message);
        REQUIRE((primary_message == "first branch failure" ||
                 primary_message == "second branch failure"));
        REQUIRE((secondary_message == "first branch failure" ||
                 secondary_message == "second branch failure"));
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
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

TEST_CASE("when_all with no tasks completes immediately",
          "[sync][combinators][regression]") {
    auto test = []() -> task<void> {
        auto result = co_await when_all();
        static_assert(std::tuple_size_v<decltype(result)> == 0);
    };

    runtime::scheduler sched(1);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("when_all does not resume while launching children",
          "[sync][combinators][regression]") {
    launch_blocker blocker;
    std::atomic<bool> resumed_during_launch{false};

    auto test = [&]() -> task<void> {
        auto ready = []() -> task<int> { co_return 1; };
        auto combo = when_all(ready, ready, blocking_move_callable(blocker));
        blocker.block_moves.store(true, std::memory_order_release);

        auto result = co_await combo;
        (void)result;
        if (blocker.move_started.load(std::memory_order_acquire) &&
            !blocker.allow_move.load(std::memory_order_acquire)) {
            resumed_during_launch.store(true, std::memory_order_release);
        }
    };

    runtime::scheduler sched(4);
    sched.start();
    sched.go(test);
    REQUIRE(wait_for_flag(blocker.move_started));
    std::this_thread::sleep_for(scaled_ms(20));
    REQUIRE_FALSE(resumed_during_launch.load(std::memory_order_acquire));
    blocker.allow_move.store(true, std::memory_order_release);
    REQUIRE(sched.shutdown(scaled_ms(2000)));
}

TEST_CASE("when_all propagates launch-time callable move failure",
          "[sync][combinators][regression]") {
    launch_throw_control control;
    std::atomic<bool> caught{false};
    std::atomic<bool> first_completed{false};

    auto test = [&]() -> task<void> {
        auto first = [&]() -> task<int> {
            first_completed.store(true, std::memory_order_release);
            co_return 1;
        };
        auto combo = when_all(first, launch_throwing_move_callable(control));
        control.block_on_move.store(true, std::memory_order_release);
        control.throw_on_move.store(true, std::memory_order_release);
        try {
            auto result = co_await combo;
            (void)result;
        } catch (const std::runtime_error& e) {
            caught.store(true, std::memory_order_release);
            REQUIRE(std::string(e.what()) == "launch move failed");
        }
    };

    runtime::scheduler sched(2);
    sched.start();
    sched.go_to(1, test);
    const bool move_started = wait_for_flag(control.move_started);
    const bool first_finished = wait_for_flag(first_completed);
    control.allow_move.store(true, std::memory_order_release);
    control.allow_move.notify_all();
    const bool did_shutdown = sched.shutdown(scaled_ms(2000));

    REQUIRE(move_started);
    REQUIRE(first_finished);
    REQUIRE(did_shutdown);
    REQUIRE(caught.load(std::memory_order_acquire));
}

TEST_CASE("when_all launch failure takes precedence over child failure",
          "[sync][combinators][regression][exception]") {
    launch_throw_control control;
    sync::event observer_ready;
    sync::event never;
    std::atomic<bool> child_failure_observed{false};
    std::atomic<bool> child_failure_reported{false};
    std::atomic<bool> completed{false};

    runtime::scheduler sched(4);
    sched.set_unhandled_exception_handler(
        [&](std::exception_ptr exception) {
            try {
                std::rethrow_exception(exception);
            } catch (const std::runtime_error& error) {
                if (std::string_view(error.what()) == "child failed first") {
                    child_failure_reported.store(
                        true, std::memory_order_release);
                }
            }
        });
    sched.start();

    sched.go_to(3, [&]() -> task<void> {
        auto first = [&]() -> task<int> {
            co_await observer_ready.wait();
            throw std::runtime_error("child failed first");
            co_return 0;
        };
        auto observer = [&]() -> task<int> {
            auto token = this_coro::cancel_token();
            [[maybe_unused]] auto cancellation_registration =
                token.on_cancel([&] {
                    child_failure_observed.store(
                        true, std::memory_order_release);
                });
            observer_ready.set();
            (void)co_await never.wait(std::move(token));
            co_return 2;
        };
        auto combo = when_all(
            first, observer, launch_throwing_move_callable(control));
        control.block_on_move.store(true, std::memory_order_release);
        control.throw_on_move.store(true, std::memory_order_release);

        try {
            (void)co_await combo;
            FAIL("expected launch failure");
        } catch (const std::runtime_error& error) {
            REQUIRE(std::string_view(error.what()) == "launch move failed");
        }
        REQUIRE(child_failure_reported.load(std::memory_order_acquire));
        completed.store(true, std::memory_order_release);
    });

    const bool move_started = wait_for_flag(control.move_started);
    const bool child_failure_recorded =
        wait_for_flag(child_failure_observed);
    control.allow_move.store(true, std::memory_order_release);
    control.allow_move.notify_all();
    const bool owner_completed = wait_for_flag(completed);
    const bool did_shutdown = sched.shutdown(scaled_ms(2000));

    REQUIRE(move_started);
    REQUIRE(child_failure_recorded);
    REQUIRE(owner_completed);
    REQUIRE(did_shutdown);
}

// --- when_any tests ---

TEST_CASE("when_any returns first completer", "[sync][combinators]") {
    auto test = []() -> task<void> {
        auto [idx, value] = co_await when_any(
            []() -> task<int> {
                co_await time::sleep_for(std::chrono::milliseconds(1));
                co_return 1;
            },
            [](coro::cancel_token token) -> task<int> {
                co_await time::sleep_for(
                    std::chrono::milliseconds(500), std::move(token));
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

TEST_CASE("when_any does not resume while launching children",
          "[sync][combinators][regression]") {
    launch_blocker blocker;
    std::atomic<bool> resumed_during_launch{false};
    std::atomic<int> resume_count{0};

    auto test = [&]() -> task<void> {
        auto ready = []() -> task<int> { co_return 1; };
        auto combo = when_any(ready, blocking_move_callable(blocker));
        blocker.block_moves.store(true, std::memory_order_release);

        auto result = co_await combo;
        (void)result;
        if (blocker.move_started.load(std::memory_order_acquire) &&
            !blocker.allow_move.load(std::memory_order_acquire)) {
            resumed_during_launch.store(true, std::memory_order_release);
        }
        resume_count.fetch_add(1, std::memory_order_release);
    };

    runtime::scheduler sched(4);
    sched.start();
    sched.go(test);
    REQUIRE(wait_for_flag(blocker.move_started));
    std::this_thread::sleep_for(scaled_ms(20));
    REQUIRE_FALSE(resumed_during_launch.load(std::memory_order_acquire));
    blocker.allow_move.store(true, std::memory_order_release);
    REQUIRE(sched.shutdown(scaled_ms(2000)));
    REQUIRE(resume_count.load(std::memory_order_acquire) == 1);
}

TEST_CASE("when_any launch failure takes precedence over selected winner",
          "[sync][combinators][regression]") {
    launch_throw_control control;
    sync::event observer_ready;
    sync::event never;
    std::atomic<bool> caught{false};
    std::atomic<bool> first_completed{false};
    std::atomic<bool> winner_cancel_observed{false};

    auto test = [&]() -> task<void> {
        auto first = [&]() -> task<int> {
            co_await observer_ready.wait();
            first_completed.store(true, std::memory_order_release);
            co_return 1;
        };
        auto observer = [&](cancel_token token) -> task<int> {
            [[maybe_unused]] auto cancellation_registration =
                token.on_cancel([&] {
                    winner_cancel_observed.store(
                        true, std::memory_order_release);
                });
            observer_ready.set();
            (void)co_await never.wait(std::move(token));
            co_return 2;
        };
        auto combo = when_any(
            first, observer, launch_throwing_move_callable(control));
        control.block_on_move.store(true, std::memory_order_release);
        control.throw_on_move.store(true, std::memory_order_release);
        try {
            auto result = co_await combo;
            (void)result;
        } catch (const std::runtime_error& e) {
            caught.store(true, std::memory_order_release);
            REQUIRE(std::string(e.what()) == "launch move failed");
        }
    };

    runtime::scheduler sched(4);
    sched.start();
    sched.go_to(3, test);
    const bool move_started = wait_for_flag(control.move_started);
    const bool winner_selected = wait_for_flag(winner_cancel_observed);
    control.allow_move.store(true, std::memory_order_release);
    control.allow_move.notify_all();
    const bool did_shutdown = sched.shutdown(scaled_ms(2000));

    REQUIRE(move_started);
    REQUIRE(winner_selected);
    REQUIRE(did_shutdown);
    REQUIRE(caught.load(std::memory_order_acquire));
    REQUIRE(first_completed.load(std::memory_order_acquire));
}

TEST_CASE("when_any second finishes first", "[sync][combinators]") {
    auto test = []() -> task<void> {
        auto [idx, value] = co_await when_any(
            [](coro::cancel_token token) -> task<int> {
                co_await time::sleep_for(
                    std::chrono::milliseconds(500), std::move(token));
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
                [](coro::cancel_token token) -> task<int> {
                    co_await time::sleep_for(
                        std::chrono::milliseconds(500), std::move(token));
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
            [&](coro::cancel_token token) -> task<void> {
                const auto result = co_await time::sleep_for(
                    std::chrono::milliseconds(500), std::move(token));
                if (result == coro::cancel_result::completed) {
                    winner.store(1, std::memory_order_relaxed);
                }
            }
        );
        REQUIRE(idx == 0);
        REQUIRE(winner.load(std::memory_order_relaxed) == 0);
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
            [](coro::cancel_token token) -> task<std::string> {
                co_await time::sleep_for(
                    std::chrono::milliseconds(500), std::move(token));
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

TEST_CASE("when_any supports non-default-constructible results",
          "[sync][combinators][regression]") {
    auto test = []() -> task<void> {
        auto [idx, value] = co_await when_any(
            []() -> task<non_default_result> {
                co_return non_default_result{42};
            },
            [](coro::cancel_token tok) -> task<non_default_result> {
                co_await time::sleep_for(std::chrono::milliseconds(500), tok);
                co_return non_default_result{-1};
            }
        );
        REQUIRE(idx == 0);
        REQUIRE(value.value == 42);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("when_any supports a non-default first heterogeneous result",
          "[sync][combinators][regression]") {
    auto test = []() -> task<void> {
        auto [idx, value] = co_await when_any(
            [](coro::cancel_token tok) -> task<non_default_result> {
                co_await time::sleep_for(std::chrono::milliseconds(500), tok);
                co_return non_default_result{-1};
            },
            []() -> task<int> {
                co_return 42;
            }
        );
        REQUIRE(idx == 1);
        REQUIRE(std::get<1>(value) == 42);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("when_any drains a losing exception before returning",
          "[sync][combinators][structured]") {
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
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("when_any loser exception triggers handler", "[sync][combinators]") {
    std::atomic<bool> handler_called{false};
    std::string handler_message;

    auto test = [&]() -> task<void> {
        auto [idx, value] = co_await when_any(
            []() -> task<int> {
                co_await time::sleep_for(std::chrono::milliseconds(1));
                co_return 42;
            },
            []() -> task<int> {
                co_await time::sleep_for(std::chrono::milliseconds(50));
                throw std::runtime_error("loser exception for handler");
                co_return 0;
            }
        );
        REQUIRE(idx == 0);
        REQUIRE(value == 42);
        REQUIRE(handler_called.load(std::memory_order_acquire));
        REQUIRE(handler_message == "loser exception for handler");
    };

    runtime::scheduler sched(2);
    sched.start();
    sched.set_unhandled_exception_handler([&](std::exception_ptr ex) {
        handler_called = true;
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            handler_message = e.what();
        }
    });
    sched.go(test);
    sched.shutdown();

    REQUIRE(handler_called.load());
    REQUIRE(handler_message == "loser exception for handler");
}

TEST_CASE("when_any winner exception propagates", "[sync][combinators]") {
    auto test = []() -> task<void> {
        bool caught = false;
        try {
            co_await when_any(
                []() -> task<int> {
                    co_await time::sleep_for(std::chrono::milliseconds(1));
                    throw std::runtime_error("winner exception");
                    co_return 0;
                },
                [](coro::cancel_token token) -> task<int> {
                    co_await time::sleep_for(
                        std::chrono::milliseconds(500), std::move(token));
                    co_return 42;
                }
            );
        } catch (const std::runtime_error& e) {
            caught = true;
            REQUIRE(std::string(e.what()) == "winner exception");
        }
        REQUIRE(caught);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("when_any publishes result construction failure",
          "[sync][combinators][regression]") {
    runtime::scheduler sched(1);
    sched.start();

    auto owner = sched.go_joinable([]() -> task<void> {
        auto first = []() -> task<int> { co_return 1; };
        auto second = []() -> task<throwing_result> {
            co_return throwing_result{2};
        };
        elio::detail::when_any_state<decltype(first), decltype(second)> state;
        task_group group;
        auto group_state =
            coro::detail::task_group_access::shared_state(group);
        throwing_result result{2};
        throwing_result::throw_on_move.store(true, std::memory_order_relaxed);

        state.resolve<1>(*group_state, std::move(result));

        REQUIRE(state.winner_claimed_.load(std::memory_order_acquire));
        REQUIRE(state.exception_ != nullptr);
        std::string exception_message;
        try {
            std::rethrow_exception(state.exception_);
        } catch (const std::exception& error) {
            exception_message = error.what();
        }
        REQUIRE(exception_message == "result transfer failed");
        co_await group.join();
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    sched.shutdown();
}

TEST_CASE("when_any preserves its winner when loser cancellation throws",
          "[sync][combinators][structured][regression]") {
    sync::event loser_ready;
    sync::event never;
    std::atomic<bool> callback_invoked{false};
    std::atomic<bool> handler_invoked{false};

    runtime::scheduler sched(2);
    sched.start();
    sched.set_unhandled_exception_handler([&](std::exception_ptr exception) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::runtime_error& error) {
            if (std::string_view(error.what()) ==
                "cancellation callback failed") {
                handler_invoked.store(true, std::memory_order_release);
            }
        }
    });

    auto owner = sched.go_joinable([&]() -> task<void> {
        auto [index, value] = co_await when_any(
            [&]() -> task<int> {
                co_await loser_ready.wait();
                co_return 42;
            },
            [&](coro::cancel_token token) -> task<int> {
                auto registration = token.on_cancel([&] {
                    callback_invoked.store(true, std::memory_order_release);
                    throw std::runtime_error("cancellation callback failed");
                });
                loser_ready.set();
                (void)co_await never.wait(token);
                co_return -1;
            });

        REQUIRE(index == 0);
        REQUIRE(value == 42);
        REQUIRE(callback_invoked.load(std::memory_order_acquire));
        REQUIRE(handler_invoked.load(std::memory_order_acquire));
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    sched.shutdown();
}

TEST_CASE("when_any waits for a token-ignoring loser to finish",
          "[sync][combinators][structured][lifetime]") {
    sync::event loser_started;
    sync::event release_loser;
    std::atomic<bool> winner_completed{false};
    std::atomic<bool> combinator_returned{false};
    runtime::scheduler sched(2);
    sched.start();

    auto owner = sched.go_joinable([&]() -> task<void> {
        auto [index, value] = co_await when_any(
            [&]() -> task<int> {
                co_await loser_started.wait();
                winner_completed.store(true, std::memory_order_release);
                co_return 42;
            },
            [&]() -> task<int> {
                loser_started.set();
                co_await release_loser.wait();
                co_return -1;
            });
        REQUIRE(index == 0);
        REQUIRE(value == 42);
        combinator_returned.store(true, std::memory_order_release);
    });

    REQUIRE(wait_for_flag(winner_completed));
    REQUIRE_FALSE(combinator_returned.load(std::memory_order_acquire));
    REQUIRE_FALSE(owner.is_ready());
    release_loser.set();
    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(combinator_returned.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("when_any drains a TCP read cancellation-completion race",
          "[sync][combinators][structured][io][tcp][cancellation]") {
    using namespace elio::net;

    auto listener = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener.has_value());
    const auto port = listener->local_address().port();

    std::optional<tcp_stream> server_stream;
    std::optional<tcp_stream> client_stream;
    std::atomic<int> setup_complete{0};
    runtime::scheduler sched(3);
    sched.start();

    sched.go([&]() -> task<void> {
        server_stream = co_await listener->accept();
        setup_complete.fetch_add(1, std::memory_order_release);
    });
    sched.go([&]() -> task<void> {
        client_stream = co_await tcp_connect(ipv6_address("::1", port));
        setup_complete.fetch_add(1, std::memory_order_release);
    });

    const auto setup_deadline =
        std::chrono::steady_clock::now() + scaled_ms(2000);
    while (setup_complete.load(std::memory_order_acquire) != 2 &&
           std::chrono::steady_clock::now() < setup_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(setup_complete.load(std::memory_order_acquire) == 2);
    REQUIRE(server_stream.has_value());
    REQUIRE(client_stream.has_value());

    sync::event read_started;
    std::atomic<bool> read_terminal{false};
    std::atomic<bool> writer_terminal{false};
    std::atomic<int> read_result{0};
    std::atomic<int> writer_result{0};

    auto writer = sched.go_joinable([&]() -> task<void> {
        co_await read_started.wait();
        co_await time::sleep_for(scaled_ms(1));
        char byte = 'x';
        const auto result = co_await client_stream->write(&byte, 1);
        writer_result.store(result.result, std::memory_order_release);
        writer_terminal.store(true, std::memory_order_release);
    });

    auto owner = sched.go_joinable([&]() -> task<void> {
        auto [index, value] = co_await when_any(
            [&](coro::cancel_token token) -> task<int> {
                (void)co_await read_started.wait(token);
                (void)co_await time::sleep_for(
                    scaled_ms(1), std::move(token));
                co_return 0;
            },
            [&](coro::cancel_token token) -> task<int> {
                char byte = 0;
                read_started.set();
                const auto result = co_await server_stream->read(
                    &byte, 1, std::move(token));
                read_result.store(result.result, std::memory_order_release);
                read_terminal.store(true, std::memory_order_release);
                co_return result.result;
            });

        REQUIRE(read_terminal.load(std::memory_order_acquire));
        if (index == 0) {
            REQUIRE(value == 0);
            const auto observed = read_result.load(std::memory_order_acquire);
            REQUIRE((observed == 1 || observed == -ECANCELED));
        } else {
            REQUIRE(index == 1);
            REQUIRE(value == 1);
        }
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    writer.wait_destroyed();
    REQUIRE_NOTHROW(writer.await_resume());
    REQUIRE(writer_terminal.load(std::memory_order_acquire));
    REQUIRE(writer_result.load(std::memory_order_acquire) == 1);
    REQUIRE(sched.shutdown(scaled_ms(5000)));
}

TEST_CASE("pre-cancelled when_all does not start child bodies",
          "[sync][combinators][structured][cancellation]") {
    sync::event continue_owner;
    std::atomic<bool> owner_waiting{false};
    std::atomic<int> child_starts{0};
    std::atomic<bool> cancellation_reported{false};
    runtime::scheduler sched(1);
    sched.start();

    auto owner = sched.go_joinable([&]() -> task<void> {
        owner_waiting.store(true, std::memory_order_release);
        co_await continue_owner.wait();
        try {
            (void)co_await when_all(
                [&]() -> task<int> {
                    child_starts.fetch_add(1, std::memory_order_release);
                    co_return 1;
                },
                [&]() -> task<int> {
                    child_starts.fetch_add(1, std::memory_order_release);
                    co_return 2;
                });
        } catch (const combinator_cancelled&) {
            cancellation_reported.store(true, std::memory_order_release);
        }
    });

    REQUIRE(wait_for_flag(owner_waiting));
    owner.request_cancel();
    continue_owner.set();
    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(child_starts.load(std::memory_order_acquire) == 0);
    REQUIRE(cancellation_reported.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("combinators return move-only lazy tasks",
          "[sync][combinators][ownership]") {
    using AllTask = decltype(when_all(
        std::declval<throwing_move_callable>()));
    using AnyTask = decltype(when_any(
        std::declval<throwing_move_callable>()));

    STATIC_REQUIRE(elio::detail::is_task_v<AllTask>);
    STATIC_REQUIRE(elio::detail::is_task_v<AnyTask>);
    STATIC_REQUIRE(std::is_move_constructible_v<AllTask>);
    STATIC_REQUIRE(std::is_move_constructible_v<AnyTask>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<AllTask>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<AnyTask>);

    std::atomic<bool> invoked{false};
    auto test = [&]() -> task<void> {
        auto pending = when_all([&]() -> task<int> {
            invoked.store(true, std::memory_order_release);
            co_return 42;
        });
        REQUIRE_FALSE(invoked.load(std::memory_order_acquire));
        auto [value] = co_await pending;
        REQUIRE(value == 42);
        REQUIRE(invoked.load(std::memory_order_acquire));
    };

    runtime::scheduler sched(1);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("when_all and when_any return move-only result values",
          "[sync][combinators][ownership]") {
    auto test = []() -> task<void> {
        auto [all_value] = co_await when_all(
            []() -> task<std::unique_ptr<int>> {
                co_return std::make_unique<int>(20);
            });
        REQUIRE(*all_value == 20);

        auto [index, any_value] = co_await when_any(
            []() -> task<std::unique_ptr<int>> {
                co_return std::make_unique<int>(22);
            },
            [](coro::cancel_token token) -> task<std::unique_ptr<int>> {
                (void)co_await time::sleep_for(
                    scaled_ms(100), std::move(token));
                co_return std::make_unique<int>(0);
            });
        REQUIRE(index == 0);
        REQUIRE(*any_value == 22);
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

TEST_CASE("with_timeout owns a move-only callable before delayed await",
          "[sync][combinators][ownership][lifetime]") {
    auto test = []() -> task<void> {
        auto pending = with_timeout(
            std::chrono::milliseconds(500),
            [owned = std::make_unique<int>(42)]() mutable -> task<int> {
                co_return *owned;
            });

        co_await time::yield();
        auto result = co_await pending;
        REQUIRE(result);
        REQUIRE(*result == 42);
    };

    runtime::scheduler sched(1);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("with_timeout supports non-default-constructible results",
          "[sync][combinators][regression]") {
    auto test = []() -> task<void> {
        auto result = co_await with_timeout(
            std::chrono::milliseconds(500),
            []() -> task<non_default_result> {
                co_return non_default_result{42};
            }
        );
        REQUIRE(result);
        REQUIRE((*result).value == 42);
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("with_timeout times out non-default-constructible results",
          "[sync][combinators][regression]") {
    auto test = []() -> task<void> {
        auto result = co_await with_timeout(
            std::chrono::milliseconds(1),
            [](coro::cancel_token tok) -> task<non_default_result> {
                co_await time::sleep_for(std::chrono::milliseconds(500), tok);
                co_return non_default_result{42};
            }
        );
        REQUIRE_FALSE(result);
        REQUIRE(result.timed_out);
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
        REQUIRE(was_cancelled.load(std::memory_order_relaxed));
    };

    runtime::scheduler sched(2);
    sched.go(test);
    sched.shutdown();
}

TEST_CASE("with_timeout waits for token-ignoring work after expiry",
          "[sync][combinators][structured][timeout][lifetime]") {
    std::atomic<bool> child_started{false};
    sync::event release_child;
    std::atomic<bool> timeout_returned{false};

    runtime::scheduler sched(2);
    sched.start();
    auto owner = sched.go_joinable([&]() -> task<void> {
        auto result = co_await with_timeout(
            scaled_ms(1),
            [&]() -> task<int> {
                child_started.store(true, std::memory_order_release);
                co_await release_child.wait();
                co_return 42;
            });
        REQUIRE(result.timed_out);
        timeout_returned.store(true, std::memory_order_release);
    });

    REQUIRE(wait_for_flag(child_started));
    std::this_thread::sleep_for(scaled_ms(20));
    REQUIRE_FALSE(timeout_returned.load(std::memory_order_acquire));
    REQUIRE_FALSE(owner.is_ready());
    release_child.set();
    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(timeout_returned.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("pre-cancelled with_timeout is not reported as expiry",
          "[sync][combinators][structured][timeout][cancellation]") {
    sync::event continue_owner;
    std::atomic<bool> owner_waiting{false};
    std::atomic<bool> cancellation_reported{false};
    std::atomic<bool> child_started{false};

    runtime::scheduler sched(1);
    sched.start();
    auto owner = sched.go_joinable([&]() -> task<void> {
        owner_waiting.store(true, std::memory_order_release);
        co_await continue_owner.wait();
        try {
            (void)co_await with_timeout(
                scaled_ms(100),
                [&]() -> task<int> {
                    child_started.store(true, std::memory_order_release);
                    co_return 42;
                });
        } catch (const combinator_cancelled&) {
            cancellation_reported.store(true, std::memory_order_release);
        }
    });

    REQUIRE(wait_for_flag(owner_waiting));
    owner.request_cancel();
    continue_owner.set();
    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(cancellation_reported.load(std::memory_order_acquire));
    REQUIRE_FALSE(child_started.load(std::memory_order_acquire));
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
