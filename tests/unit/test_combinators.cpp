#include <catch2/catch_test_macros.hpp>
#include <elio/elio.hpp>
#include <elio/coro/when_all.hpp>
#include <elio/coro/when_any.hpp>
#include <elio/coro/with_timeout.hpp>
#include <elio/time/timer.hpp>
#include <atomic>
#include <barrier>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include "../test_main.cpp"

using namespace elio;
using namespace elio::coro;
using namespace elio::test;

namespace {

bool wait_for_count(const std::atomic<int>& value, int expected) {
    const auto deadline = std::chrono::steady_clock::now() + scaled_ms(2000);
    while (value.load(std::memory_order_acquire) != expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return value.load(std::memory_order_acquire) == expected;
}

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

struct launch_throw_control {
    std::atomic<bool> throw_on_move{false};
    const std::atomic<bool>* wait_before_throw{nullptr};
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
        if (control && control->throw_on_move.load(std::memory_order_acquire)) {
            const auto* wait_flag = control->wait_before_throw;
            const auto deadline =
                std::chrono::steady_clock::now() + scaled_ms(2000);
            while (wait_flag && !wait_flag->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
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

struct resume_probe {
    struct promise_type {
        resume_probe get_return_object() noexcept {
            return resume_probe{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };

    explicit resume_probe(std::coroutine_handle<promise_type> handle) noexcept
        : handle_(handle) {}

    resume_probe(resume_probe&& other) noexcept
        : handle_(other.handle_) {
        other.handle_ = {};
    }

    resume_probe(const resume_probe&) = delete;
    resume_probe& operator=(const resume_probe&) = delete;

    ~resume_probe() {
        if (handle_) {
            handle_.destroy();
        }
    }

    std::coroutine_handle<> handle() const noexcept { return handle_; }

private:
    std::coroutine_handle<promise_type> handle_;
};

resume_probe make_resume_probe(std::atomic<int>& resumes) {
    co_await std::suspend_always{};
    resumes.fetch_add(1, std::memory_order_release);
}

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
    control.wait_before_throw = &first_completed;

    auto test = [&]() -> task<void> {
        auto first = [&]() -> task<int> {
            first_completed.store(true, std::memory_order_release);
            co_return 1;
        };
        auto combo = when_all(first, launch_throwing_move_callable(control));
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
    sched.go(test);
    REQUIRE(sched.shutdown(scaled_ms(2000)));
    REQUIRE(caught.load(std::memory_order_acquire));
    REQUIRE(first_completed.load(std::memory_order_acquire));
}

TEST_CASE("when_all state wakes when completion follows launch",
          "[sync][combinators][regression]") {
    auto child = []() -> task<int> { co_return 1; };
    using state_t = elio::detail::when_all_state<decltype(child)>;

    state_t state(1);
    coro::detail::completion_waiter waiter(state.waiter_);
    std::atomic<int> resumes{0};
    auto probe = make_resume_probe(resumes);

    REQUIRE(state.set_waiter(waiter, probe.handle()));
    state.finish_launching();
    REQUIRE(resumes.load(std::memory_order_acquire) == 0);

    state.complete_one();
    REQUIRE(resumes.load(std::memory_order_acquire) == 1);
}

TEST_CASE("when_all state wakes when launch follows completion",
          "[sync][combinators][regression]") {
    auto child = []() -> task<int> { co_return 1; };
    using state_t = elio::detail::when_all_state<decltype(child)>;

    state_t state(1);
    coro::detail::completion_waiter waiter(state.waiter_);
    std::atomic<int> resumes{0};
    auto probe = make_resume_probe(resumes);

    REQUIRE(state.set_waiter(waiter, probe.handle()));
    state.complete_one();
    REQUIRE(resumes.load(std::memory_order_acquire) == 0);

    state.finish_launching();
    REQUIRE(resumes.load(std::memory_order_acquire) == 1);
}

TEST_CASE("when_all state wakes after unlaunched children are accounted",
          "[sync][combinators][regression]") {
    auto child = []() -> task<int> { co_return 1; };
    using state_t =
        elio::detail::when_all_state<decltype(child), decltype(child)>;

    state_t state(2);
    coro::detail::completion_waiter waiter(state.waiter_);
    std::atomic<int> resumes{0};
    auto probe = make_resume_probe(resumes);

    REQUIRE(state.set_waiter(waiter, probe.handle()));
    state.complete_one();
    state.complete_unlaunched(1);
    REQUIRE(resumes.load(std::memory_order_acquire) == 0);

    state.finish_launching();
    REQUIRE(resumes.load(std::memory_order_acquire) == 1);
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

TEST_CASE("when_any propagates launch-time callable move failure",
          "[sync][combinators][regression]") {
    launch_throw_control control;
    std::atomic<bool> caught{false};
    std::atomic<bool> first_completed{false};
    control.wait_before_throw = &first_completed;

    auto test = [&]() -> task<void> {
        auto first = [&]() -> task<int> {
            first_completed.store(true, std::memory_order_release);
            co_return 1;
        };
        auto combo = when_any(first, launch_throwing_move_callable(control));
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
    sched.go(test);
    REQUIRE(sched.shutdown(scaled_ms(2000)));
    REQUIRE(caught.load(std::memory_order_acquire));
    REQUIRE(first_completed.load(std::memory_order_acquire));
}

TEST_CASE("when_any wake gate claims one resume in either publication order",
          "[sync][combinators][regression]") {
    SECTION("resolution is published first") {
        elio::detail::when_any_wake_gate gate;
        REQUIRE_FALSE(gate.publish_resolved());
        REQUIRE(gate.resolved());
        REQUIRE(gate.finish_launching());
        REQUIRE_FALSE(gate.publish_resolved());
        REQUIRE_FALSE(gate.finish_launching());
    }

    SECTION("launch completion is published first") {
        elio::detail::when_any_wake_gate gate;
        REQUIRE_FALSE(gate.finish_launching());
        REQUIRE_FALSE(gate.resolved());
        REQUIRE(gate.publish_resolved());
        REQUIRE(gate.resolved());
        REQUIRE_FALSE(gate.finish_launching());
        REQUIRE_FALSE(gate.publish_resolved());
    }
}

TEST_CASE("when_any wake gate synchronizes concurrent publications",
          "[sync][combinators][regression]") {
    constexpr std::size_t rounds = 1000;
    auto gates = std::make_unique<elio::detail::when_any_wake_gate[]>(rounds);
    auto resolved_claimed = std::make_unique<bool[]>(rounds);
    auto launch_claimed = std::make_unique<bool[]>(rounds);
    std::barrier rendezvous(3);

    {
        std::jthread resolved_thread([&] {
            for (std::size_t i = 0; i < rounds; ++i) {
                rendezvous.arrive_and_wait();
                resolved_claimed[i] = gates[i].publish_resolved();
                rendezvous.arrive_and_wait();
            }
        });
        std::jthread launch_thread([&] {
            for (std::size_t i = 0; i < rounds; ++i) {
                rendezvous.arrive_and_wait();
                launch_claimed[i] = gates[i].finish_launching();
                rendezvous.arrive_and_wait();
            }
        });

        for (std::size_t i = 0; i < rounds; ++i) {
            rendezvous.arrive_and_wait();
            rendezvous.arrive_and_wait();
        }
    }

    for (std::size_t i = 0; i < rounds; ++i) {
        REQUIRE(resolved_claimed[i] != launch_claimed[i]);
    }
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

TEST_CASE("when_any loser exception is logged", "[sync][combinators]") {
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
        co_await time::sleep_for(std::chrono::milliseconds(200));
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
                []() -> task<int> {
                    co_await time::sleep_for(std::chrono::milliseconds(500));
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
    auto first = []() -> task<int> { co_return 1; };
    auto second = []() -> task<throwing_result> {
        co_return throwing_result{2};
    };
    elio::detail::when_any_state<decltype(first), decltype(second)> state;
    throwing_result result{2};
    throwing_result::throw_on_move.store(true, std::memory_order_relaxed);

    state.resolve<1>(std::move(result));

    REQUIRE(state.winner_claimed_.load(std::memory_order_acquire));
    REQUIRE(state.wake_gate_.resolved());
    REQUIRE(state.exception_ != nullptr);
    std::string exception_message;
    try {
        std::rethrow_exception(state.exception_);
    } catch (const std::exception& e) {
        exception_message = e.what();
    }
    REQUIRE(exception_message == "result transfer failed");
}

TEST_CASE("when_any publishes winner when loser cancellation throws",
          "[sync][combinators][regression]") {
    auto first = []() -> task<int> { co_return 1; };
    auto second = []() -> task<int> { co_return 2; };
    elio::detail::when_any_state<decltype(first), decltype(second)> state;
    bool callback_invoked = false;
    auto registration = state.cancel_source_.get_token().on_cancel([&] {
        callback_invoked = true;
        throw std::runtime_error("cancellation callback failed");
    });

    state.resolve<0>(42);

    REQUIRE(callback_invoked);
    REQUIRE(state.wake_gate_.resolved());
    REQUIRE(state.exception_ == nullptr);
    REQUIRE(state.result_.has_value());
    REQUIRE(std::get<0>(*state.result_) == 42);
}

TEST_CASE("destroyed when_all waiter is unregistered",
          "[sync][combinators][cancellation]") {
    sync::event gate;
    std::atomic<int> started{0};
    std::atomic<int> completed{0};

    auto child = [&]() -> task<void> {
        started.fetch_add(1, std::memory_order_release);
        co_await gate.wait();
        completed.fetch_add(1, std::memory_order_release);
    };
    auto outer = [&]() -> task<void> {
        co_await when_all(child, child);
    };

    runtime::scheduler sched(2);
    sched.start();
    auto outer_task = outer();
    auto h = elio::coro::detail::task_access::release(outer_task);
    sched.spawn(h);

    REQUIRE(wait_for_count(started, 2));
    h.destroy();
    sched.go([&]() -> task<void> {
        gate.set();
        co_return;
    });
    REQUIRE(wait_for_count(completed, 2));
    sched.shutdown();
}

TEST_CASE("destroyed when_any waiter is unregistered",
          "[sync][combinators][cancellation]") {
    sync::event gate;
    std::atomic<int> started{0};
    std::atomic<int> completed{0};

    auto child = [&]() -> task<int> {
        started.fetch_add(1, std::memory_order_release);
        co_await gate.wait();
        completed.fetch_add(1, std::memory_order_release);
        co_return 1;
    };
    auto outer = [&]() -> task<void> {
        (void)co_await when_any(child, child);
    };

    runtime::scheduler sched(2);
    sched.start();
    auto outer_task = outer();
    auto h = elio::coro::detail::task_access::release(outer_task);
    sched.spawn(h);

    REQUIRE(wait_for_count(started, 2));
    h.destroy();
    sched.go([&]() -> task<void> {
        gate.set();
        co_return;
    });
    REQUIRE(wait_for_count(completed, 2));
    sched.shutdown();
}

TEST_CASE("combinator awaitable move traits follow callables",
          "[sync][combinators]") {
    using AllAwaitable = elio::detail::when_all_awaitable<throwing_move_callable>;
    using AnyAwaitable = elio::detail::when_any_awaitable<throwing_move_callable>;

    STATIC_REQUIRE(std::is_move_constructible_v<AllAwaitable>);
    STATIC_REQUIRE(std::is_move_constructible_v<AnyAwaitable>);
    STATIC_REQUIRE_FALSE(std::is_nothrow_move_constructible_v<AllAwaitable>);
    STATIC_REQUIRE_FALSE(std::is_nothrow_move_constructible_v<AnyAwaitable>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<AllAwaitable>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<AnyAwaitable>);
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
