#include <catch2/catch_test_macros.hpp>
#include <elio/elio.hpp>

#include <atomic>
#include <climits>
#include <chrono>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

using elio::coro::cancel_result;
using elio::coro::task;
using elio::coro::task_group;
using elio::coro::task_group_error;
using elio::coro::task_group_failure_policy;
using elio::coro::task_group_options;
using elio::runtime::scheduler;

namespace {

template<typename Predicate>
bool wait_for_condition(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return predicate();
}

bool wait_for_flag(const std::atomic<bool>& flag) {
    return wait_for_condition([&] {
        return flag.load(std::memory_order_acquire);
    });
}

void update_peak(std::atomic<int>& peak, int value) {
    int observed = peak.load(std::memory_order_relaxed);
    while (observed < value &&
           !peak.compare_exchange_weak(
               observed, value, std::memory_order_relaxed)) {}
}

class lifetime_probe final {
public:
    explicit lifetime_probe(std::atomic<bool>& destroyed) noexcept
        : destroyed_(&destroyed) {}

    lifetime_probe(lifetime_probe&& other) noexcept
        : destroyed_(std::exchange(other.destroyed_, nullptr)) {}

    lifetime_probe& operator=(lifetime_probe&&) = delete;
    lifetime_probe(const lifetime_probe&) = delete;
    lifetime_probe& operator=(const lifetime_probe&) = delete;

    ~lifetime_probe() {
        if (destroyed_) {
            destroyed_->store(true, std::memory_order_release);
        }
    }

    task<void> operator()() {
        co_return;
    }

private:
    std::atomic<bool>* destroyed_;
};

class scope_body_lifetime_probe final {
public:
    scope_body_lifetime_probe(
        elio::sync::event& child_started,
        elio::sync::event& release_child,
        std::atomic<bool>& body_returned,
        std::atomic<bool>& callable_destroyed,
        std::atomic<bool>& child_observed_callable_alive) noexcept
        : child_started_(&child_started)
        , release_child_(&release_child)
        , body_returned_(&body_returned)
        , callable_destroyed_(&callable_destroyed)
        , child_observed_callable_alive_(&child_observed_callable_alive) {}

    scope_body_lifetime_probe(scope_body_lifetime_probe&& other) noexcept
        : child_started_(other.child_started_)
        , release_child_(other.release_child_)
        , body_returned_(other.body_returned_)
        , callable_destroyed_(
              std::exchange(other.callable_destroyed_, nullptr))
        , child_observed_callable_alive_(
              other.child_observed_callable_alive_) {}

    scope_body_lifetime_probe& operator=(scope_body_lifetime_probe&&) = delete;
    scope_body_lifetime_probe(const scope_body_lifetime_probe&) = delete;
    scope_body_lifetime_probe& operator=(
        const scope_body_lifetime_probe&) = delete;

    ~scope_body_lifetime_probe() {
        if (callable_destroyed_) {
            callable_destroyed_->store(true, std::memory_order_release);
        }
    }

    task<void> operator()(task_group& group) {
        auto* child_started = child_started_;
        auto* release_child = release_child_;
        auto* callable_destroyed = callable_destroyed_;
        auto* child_observed_callable_alive =
            child_observed_callable_alive_;

        group.spawn(
            [child_started, release_child, callable_destroyed,
             child_observed_callable_alive]() -> task<void> {
                child_started->set();
                co_await release_child->wait();
                child_observed_callable_alive->store(
                    !callable_destroyed->load(std::memory_order_acquire),
                    std::memory_order_release);
            });
        body_returned_->store(true, std::memory_order_release);
        co_return;
    }

private:
    elio::sync::event* child_started_;
    elio::sync::event* release_child_;
    std::atomic<bool>* body_returned_;
    std::atomic<bool>* callable_destroyed_;
    std::atomic<bool>* child_observed_callable_alive_;
};

class inline_resume_gate final {
public:
    class awaitable final {
    public:
        explicit awaitable(inline_resume_gate& gate) noexcept : gate_(gate) {}

        [[nodiscard]] bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> handle) noexcept {
            gate_.handle_.store(handle.address(), std::memory_order_release);
        }

        void await_resume() const noexcept {}

    private:
        inline_resume_gate& gate_;
    };

    [[nodiscard]] awaitable wait() noexcept { return awaitable(*this); }

    [[nodiscard]] bool is_waiting() const noexcept {
        return handle_.load(std::memory_order_acquire) != nullptr;
    }

    void resume_inline() {
        auto* address = handle_.exchange(nullptr, std::memory_order_acq_rel);
        if (!address) {
            throw std::logic_error("inline resume gate has no waiter");
        }
        std::coroutine_handle<>::from_address(address).resume();
    }

private:
    std::atomic<void*> handle_{nullptr};
};

} // namespace

TEST_CASE("task_group joins children in one scheduler domain",
          "[task_group][structured]") {
    scheduler sched(2);
    sched.start();
    std::atomic<int> sum{0};
    std::atomic<bool> same_domain{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        task_group group;
        same_domain.store(
            &group.scheduler_domain() == scheduler::current(),
            std::memory_order_release);

        for (int value = 1; value <= 4; ++value) {
            group.spawn([&, value]() -> task<int> {
                sum.fetch_add(value, std::memory_order_relaxed);
                co_return value;
            });
        }

        co_await group.join();
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(same_domain.load(std::memory_order_acquire));
    REQUIRE(sum.load(std::memory_order_relaxed) == 10);
    sched.shutdown();
}

TEST_CASE("task_group fail-fast cancels unfinished siblings",
          "[task_group][structured][cancellation][failure]") {
    scheduler sched(3);
    sched.start();
    elio::sync::event sibling_started;
    elio::sync::event never;
    std::atomic<bool> sibling_cancelled{false};
    std::atomic<bool> failure_propagated{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        task_group group;
        group.spawn([&]() -> task<void> {
            co_await sibling_started.wait();
            throw std::runtime_error("child failure");
        });
        group.spawn([&]() -> task<void> {
            sibling_started.set();
            const auto result = co_await never.wait(
                elio::coro::this_coro::cancel_token());
            sibling_cancelled.store(
                result == cancel_result::cancelled,
                std::memory_order_release);
        });

        try {
            co_await group.join();
        } catch (const std::runtime_error& error) {
            failure_propagated.store(
                std::string_view(error.what()) == "child failure",
                std::memory_order_release);
        }
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(failure_propagated.load(std::memory_order_acquire));
    REQUIRE(sibling_cancelled.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("task_group collect-all records failures without sibling cancellation",
          "[task_group][structured][failure]") {
    scheduler sched(3);
    sched.start();
    std::atomic<int> completed{0};
    std::atomic<size_t> failure_count{0};
    std::atomic<bool> cancellation_seen{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        task_group group(task_group_options{
            .failure_policy = task_group_failure_policy::collect_all});
        group.spawn([&]() -> task<void> {
            completed.fetch_add(1, std::memory_order_relaxed);
            throw std::runtime_error("first");
        });
        group.spawn([&]() -> task<void> {
            cancellation_seen.store(
                elio::coro::this_coro::cancel_token().is_cancelled(),
                std::memory_order_release);
            completed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        });
        group.spawn([&]() -> task<void> {
            completed.fetch_add(1, std::memory_order_relaxed);
            throw std::logic_error("second");
        });

        try {
            co_await group.join();
        } catch (const task_group_error& error) {
            failure_count.store(
                error.failures().size(), std::memory_order_release);
        }
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(completed.load(std::memory_order_relaxed) == 3);
    REQUIRE(failure_count.load(std::memory_order_acquire) == 2);
    REQUIRE_FALSE(cancellation_seen.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("task_group enforces bounded child-body concurrency",
          "[task_group][structured][bounded]") {
    scheduler sched(4);
    sched.start();
    elio::sync::event release_children;
    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    std::atomic<int> started{0};
    std::atomic<int> completed{0};

    auto owner = sched.go_joinable([&]() -> task<void> {
        task_group group(task_group_options{.max_concurrency = 2});
        for (int i = 0; i < 8; ++i) {
            group.spawn([&]() -> task<void> {
                const int now = active.fetch_add(
                    1, std::memory_order_acq_rel) + 1;
                update_peak(peak, now);
                started.fetch_add(1, std::memory_order_release);
                co_await release_children.wait();
                active.fetch_sub(1, std::memory_order_acq_rel);
                completed.fetch_add(1, std::memory_order_release);
            });
        }

        while (started.load(std::memory_order_acquire) < 2) {
            co_await elio::time::yield();
        }
        release_children.set();
        co_await group.join();
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(peak.load(std::memory_order_relaxed) == 2);
    REQUIRE(completed.load(std::memory_order_acquire) == 8);
    sched.shutdown();
}

TEST_CASE("task_group bounded child survives one permit wake rejection",
          "[task_group][structured][bounded][scheduler][failure]") {
    scheduler sched(1);
    sched.start();
    elio::sync::event first_started;
    elio::sync::event release_first;
    std::atomic<bool> second_started{false};
    const auto waiter_publications_before =
        elio::sync::detail::semaphore_waiter_publications_for_test.load(
            std::memory_order_acquire);
    const auto local_fallbacks_before =
        elio::runtime::detail::local_schedule_fallbacks_for_test.load(
            std::memory_order_acquire);

    auto owner = sched.go_joinable([&]() -> task<void> {
        task_group group(task_group_options{.max_concurrency = 1});
        group.spawn([&]() -> task<void> {
            first_started.set();
            co_await release_first.wait();
            elio::runtime::detail::reject_next_schedule_for_test.store(
                true, std::memory_order_release);
        });
        co_await first_started.wait();
        group.spawn([&]() -> task<void> {
            second_started.store(true, std::memory_order_release);
            co_return;
        });
        co_await group.join();
    });

    REQUIRE(wait_for_condition([&] { return first_started.is_set(); }));
    REQUIRE(wait_for_condition([&] {
        return elio::sync::detail::semaphore_waiter_publications_for_test.load(
                   std::memory_order_acquire) > waiter_publications_before;
    }));
    release_first.set();
    REQUIRE(wait_for_flag(second_started));
    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE_FALSE(
        elio::runtime::detail::reject_next_schedule_for_test.load(
            std::memory_order_acquire));
    REQUIRE(
        elio::runtime::detail::local_schedule_fallbacks_for_test.load(
            std::memory_order_acquire) > local_fallbacks_before);
    sched.shutdown();
}

TEST_CASE("task_group drains a permit wake during worker force shutdown",
          "[task_group][structured][bounded][scheduler][shutdown]") {
    scheduler sched(1);
    sched.start();
    elio::sync::event first_started;
    elio::sync::event release_first;
    std::atomic<bool> second_started{false};
    const auto waiter_publications_before =
        elio::sync::detail::semaphore_waiter_publications_for_test.load(
            std::memory_order_acquire);
    const auto local_fallbacks_before =
        elio::runtime::detail::local_schedule_fallbacks_for_test.load(
            std::memory_order_acquire);

    auto owner = sched.go_joinable([&]() -> task<void> {
        task_group group(task_group_options{.max_concurrency = 1});
        group.spawn([&]() -> task<void> {
            first_started.set();
            co_await release_first.wait();
            sched.shutdown_force();
            co_return;
        });
        co_await first_started.wait();
        group.spawn([&]() -> task<void> {
            second_started.store(true, std::memory_order_release);
            co_return;
        });
        co_await group.join();
    });

    REQUIRE(wait_for_condition([&] { return first_started.is_set(); }));
    REQUIRE(wait_for_condition([&] {
        return elio::sync::detail::semaphore_waiter_publications_for_test.load(
                   std::memory_order_acquire) > waiter_publications_before;
    }));
    release_first.set();
    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(second_started.load(std::memory_order_acquire));
    REQUIRE(
        elio::runtime::detail::local_schedule_fallbacks_for_test.load(
            std::memory_order_acquire) > local_fallbacks_before);
}

TEST_CASE("parent cancellation propagates through task_group children",
          "[task_group][structured][cancellation]") {
    scheduler sched(2);
    sched.start();
    elio::sync::event never;
    std::atomic<bool> child_started{false};
    std::atomic<bool> child_cancelled{false};
    std::atomic<bool> child_resumed_on_domain{false};
    std::atomic<bool> continuation_on_domain{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        task_group group;
        group.spawn([&]() -> task<void> {
            child_started.store(true, std::memory_order_release);
            const auto result = co_await never.wait(
                elio::coro::this_coro::cancel_token());
            child_cancelled.store(
                result == cancel_result::cancelled,
                std::memory_order_release);
            child_resumed_on_domain.store(
                scheduler::current() == &sched, std::memory_order_release);
        });
        co_await group.join();
        continuation_on_domain.store(
            scheduler::current() == &sched, std::memory_order_release);
    });

    REQUIRE(wait_for_flag(child_started));
    owner.request_cancel();
    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(child_cancelled.load(std::memory_order_acquire));
    REQUIRE(child_resumed_on_domain.load(std::memory_order_acquire));
    REQUIRE(continuation_on_domain.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("task_group cancellation propagates through nested groups",
          "[task_group][structured][nested][cancellation][failure]") {
    scheduler sched(4);
    sched.start();
    elio::sync::event inner_started;
    elio::sync::event never;
    std::atomic<bool> inner_cancelled{false};
    std::atomic<bool> outer_failed{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        task_group outer;
        outer.spawn([&]() -> task<void> {
            task_group inner;
            inner.spawn([&]() -> task<void> {
                inner_started.set();
                const auto result = co_await never.wait(
                    elio::coro::this_coro::cancel_token());
                inner_cancelled.store(
                    result == cancel_result::cancelled,
                    std::memory_order_release);
            });
            co_await inner.join();
        });
        outer.spawn([&]() -> task<void> {
            co_await inner_started.wait();
            throw std::runtime_error("outer failure");
        });

        try {
            co_await outer.join();
        } catch (const std::runtime_error&) {
            outer_failed.store(true, std::memory_order_release);
        }
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(outer_failed.load(std::memory_order_acquire));
    REQUIRE(inner_cancelled.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("task_scope cancels and joins children when its body fails",
          "[task_group][task_scope][structured][lifetime]") {
    scheduler sched(2);
    sched.start();
    elio::sync::event child_started;
    elio::sync::event never;
    std::atomic<bool> child_cancelled{false};
    std::atomic<bool> body_failure_propagated{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        try {
            co_await elio::coro::task_scope(
                [&](task_group& group) -> task<void> {
                    group.spawn([&]() -> task<void> {
                        child_started.set();
                        const auto result = co_await never.wait(
                            elio::coro::this_coro::cancel_token());
                        child_cancelled.store(
                            result == cancel_result::cancelled,
                            std::memory_order_release);
                    });
                    co_await child_started.wait();
                    throw std::runtime_error("scope body failure");
                });
        } catch (const std::runtime_error& error) {
            body_failure_propagated.store(
                std::string_view(error.what()) == "scope body failure",
                std::memory_order_release);
        }
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(body_failure_propagated.load(std::memory_order_acquire));
    REQUIRE(child_cancelled.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("task_scope fail-fast cancellation reaches its suspended body",
          "[task_group][task_scope][structured][cancellation][failure]") {
    scheduler sched(3);
    sched.start();
    elio::sync::event body_waiting;
    elio::sync::event never;
    std::atomic<bool> body_cancelled{false};
    std::atomic<bool> child_failure_propagated{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        try {
            co_await elio::coro::task_scope(
                [&](task_group& group) -> task<void> {
                    group.spawn([&]() -> task<void> {
                        co_await body_waiting.wait();
                        throw std::runtime_error("scope child failure");
                    });
                    body_waiting.set();
                    const auto result = co_await never.wait(
                        elio::coro::this_coro::cancel_token());
                    body_cancelled.store(
                        result == cancel_result::cancelled,
                        std::memory_order_release);
                });
        } catch (const std::runtime_error& error) {
            child_failure_propagated.store(
                std::string_view(error.what()) == "scope child failure",
                std::memory_order_release);
        }
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(body_cancelled.load(std::memory_order_acquire));
    REQUIRE(child_failure_propagated.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("task_scope collect-all preserves every child failure",
          "[task_group][task_scope][structured][failure]") {
    scheduler sched(3);
    sched.start();
    std::atomic<size_t> collected{0};

    auto owner = sched.go_joinable([&]() -> task<void> {
        try {
            co_await elio::coro::task_scope(
                [](task_group& group) -> task<void> {
                    group.spawn([]() -> task<void> {
                        throw std::runtime_error("first");
                    });
                    group.spawn([]() -> task<void> {
                        throw std::logic_error("second");
                    });
                    co_return;
                },
                {.failure_policy = task_group_failure_policy::collect_all});
        } catch (const task_group_error& error) {
            collected.store(
                error.failures().size(), std::memory_order_release);
        }
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(collected.load(std::memory_order_acquire) == 2);
    sched.shutdown();
}

TEST_CASE("task_scope collect-all combines body and child failures",
          "[task_group][task_scope][structured][failure]") {
    scheduler sched(2);
    sched.start();
    std::atomic<size_t> collected{0};

    auto owner = sched.go_joinable([&]() -> task<void> {
        try {
            co_await elio::coro::task_scope(
                [](task_group& group) -> task<void> {
                    group.spawn([]() -> task<void> {
                        throw std::runtime_error("child failure");
                    });
                    while (group.failures().empty()) {
                        co_await elio::time::yield();
                    }
                    throw std::logic_error("body failure");
                },
                {.failure_policy = task_group_failure_policy::collect_all});
        } catch (const task_group_error& error) {
            collected.store(
                error.failures().size(), std::memory_order_release);
        }
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(collected.load(std::memory_order_acquire) == 2);
    sched.shutdown();
}

TEST_CASE("task_scope accepts a generic lvalue group callable",
          "[task_group][task_scope][structured][contract]") {
    scheduler sched(1);
    sched.start();
    std::atomic<bool> child_completed{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        co_await elio::coro::task_scope(
            [&](auto& group) -> task<void> {
                group.spawn([&]() -> task<void> {
                    child_completed.store(true, std::memory_order_release);
                    co_return;
                });
                co_return;
            });
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(child_completed.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("task_scope child count excludes its body",
          "[task_group][task_scope][structured][contract]") {
    scheduler sched(1);
    sched.start();
    elio::sync::event release_child;
    std::atomic<bool> observed_empty{false};
    std::atomic<bool> observed_one_child{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        co_await elio::coro::task_scope(
            [&](task_group& group) -> task<void> {
                observed_empty.store(
                    group.outstanding_children() == 0,
                    std::memory_order_release);
                group.spawn([&]() -> task<void> {
                    co_await release_child.wait();
                });
                observed_one_child.store(
                    group.outstanding_children() == 1,
                    std::memory_order_release);
                release_child.set();
                co_return;
            });
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(observed_empty.load(std::memory_order_acquire));
    REQUIRE(observed_one_child.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("task_scope retains its body callable until children drain",
          "[task_group][task_scope][structured][lifetime]") {
    scheduler sched(1);
    sched.start();
    elio::sync::event child_started;
    elio::sync::event release_child;
    std::atomic<bool> body_returning{false};
    std::atomic<bool> body_callable_destroyed{false};
    std::atomic<bool> child_observed_callable_alive{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        co_await elio::coro::task_scope(
            scope_body_lifetime_probe(
                child_started, release_child, body_returning,
                body_callable_destroyed, child_observed_callable_alive));
    });

    REQUIRE(wait_for_condition([&] { return child_started.is_set(); }));
    REQUIRE(wait_for_flag(body_returning));
    release_child.set();
    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(child_observed_callable_alive.load(std::memory_order_acquire));
    REQUIRE(body_callable_destroyed.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("task_scope survives rejected handoff after external body wakeup",
          "[task_group][task_scope][structured][scheduler_domain][failure]") {
    scheduler sched(1);
    sched.start();
    inline_resume_gate release_body;
    elio::sync::event child_started;
    elio::sync::event release_child;
    std::atomic<bool> body_resumed_externally{false};
    std::atomic<bool> scope_continuation_on_domain{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        co_await elio::coro::task_scope(
            [&](task_group& group) -> task<void> {
                group.spawn([&]() -> task<void> {
                    child_started.set();
                    co_await release_child.wait();
                });
                co_await release_body.wait();
                body_resumed_externally.store(
                    scheduler::current() == nullptr,
                    std::memory_order_release);
            });
        scope_continuation_on_domain.store(
            scheduler::current() == &sched, std::memory_order_release);
    });

    REQUIRE(wait_for_condition([&] { return release_body.is_waiting(); }));
    REQUIRE(wait_for_condition([&] { return child_started.is_set(); }));
    std::thread external_resumer([&] {
        elio::runtime::detail::reject_next_schedule_for_test.store(
            true, std::memory_order_release);
        release_body.resume_inline();
    });
    external_resumer.join();
    REQUIRE_FALSE(
        elio::runtime::detail::reject_next_schedule_for_test.load(
            std::memory_order_acquire));
    release_child.set();
    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(body_resumed_externally.load(std::memory_order_acquire));
    REQUIRE(scope_continuation_on_domain.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("task_scope hands off from the scheduler start caller",
          "[task_group][task_scope][structured][scheduler_domain]") {
    scheduler sched(1);
    sched.start();
    inline_resume_gate release_body;
    std::atomic<bool> body_resumed_on_start_caller{false};
    std::atomic<bool> scope_continuation_on_worker{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        co_await elio::coro::task_scope(
            sched,
            [&](task_group&) -> task<void> {
                co_await release_body.wait();
                body_resumed_on_start_caller.store(
                    scheduler::current() == &sched &&
                        elio::runtime::worker_thread::current() == nullptr,
                    std::memory_order_release);
            });
        scope_continuation_on_worker.store(
            scheduler::current() == &sched &&
                elio::runtime::worker_thread::current() != nullptr,
            std::memory_order_release);
    });

    REQUIRE(wait_for_condition([&] { return release_body.is_waiting(); }));
    release_body.resume_inline();
    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(body_resumed_on_start_caller.load(std::memory_order_acquire));
    REQUIRE(scope_continuation_on_worker.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("task_scope body cannot join its own group",
          "[task_group][task_scope][structured][contract]") {
    scheduler sched(1);
    sched.start();
    std::atomic<bool> self_join_rejected{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        try {
            co_await elio::coro::task_scope(
                [](task_group& group) -> task<void> {
                    co_await group.join();
                });
        } catch (const std::logic_error& error) {
            self_join_rejected.store(
                std::string_view(error.what()).find("cannot join") !=
                    std::string_view::npos,
                std::memory_order_release);
        }
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(self_join_rejected.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("destroying an unjoined task_group requests child cancellation",
          "[task_group][structured][cancellation][lifetime]") {
    scheduler sched(2);
    sched.start();
    elio::sync::event never;
    std::atomic<bool> child_started{false};
    std::atomic<bool> child_cancelled{false};

    {
        task_group group(sched);
        group.spawn([&]() -> task<void> {
            child_started.store(true, std::memory_order_release);
            const auto result = co_await never.wait(
                elio::coro::this_coro::cancel_token());
            child_cancelled.store(
                result == cancel_result::cancelled,
                std::memory_order_release);
        });
        REQUIRE(wait_for_flag(child_started));
    }

    REQUIRE(wait_for_flag(child_cancelled));
    REQUIRE(sched.wait_for_idle(std::chrono::seconds(5)));
    sched.shutdown();
}

TEST_CASE("task_group validates construction and join lifecycle",
          "[task_group][structured][contract]") {
    REQUIRE_THROWS_AS(task_group{}, std::logic_error);

    scheduler sched(1);
    REQUIRE_THROWS_AS(
        task_group(sched, task_group_options{
            .max_concurrency = static_cast<size_t>(INT_MAX) + 1}),
        std::invalid_argument);

    sched.start();
    std::atomic<bool> rejected_after_join{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        task_group group;
        co_await group.join();
        try {
            group.spawn([]() -> task<void> { co_return; });
        } catch (const std::logic_error&) {
            rejected_after_join.store(true, std::memory_order_release);
        }
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(rejected_after_join.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("task_group reports scheduler rejection through join",
          "[task_group][structured][failure][contract]") {
    scheduler sched(1);
    task_group group(sched);
    group.spawn([]() -> task<void> { co_return; });
    REQUIRE(group.outstanding_children() == 0);

    sched.start();
    std::atomic<bool> rejection_propagated{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        try {
            co_await group.join();
        } catch (const std::logic_error& error) {
            rejection_propagated.store(
                std::string_view(error.what()).find("scheduler rejected") !=
                    std::string_view::npos,
                std::memory_order_release);
        }
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(rejection_propagated.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("task_group rejection cancellation permits reentrant spawn",
          "[task_group][structured][failure][cancellation][reentrant]") {
    scheduler sched(1);
    sched.start();
    elio::sync::event callback_ready;
    elio::sync::event never;
    std::atomic<bool> callback_reentered{false};
    std::atomic<bool> rejection_propagated{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        task_group group;
        group.spawn([&]() -> task<void> {
            auto registration =
                elio::coro::this_coro::cancel_token().on_cancel([&] {
                    group.spawn([]() -> task<void> { co_return; });
                    callback_reentered.store(true, std::memory_order_release);
                });
            callback_ready.set();
            (void)co_await never.wait(
                elio::coro::this_coro::cancel_token());
        });

        co_await callback_ready.wait();
        elio::runtime::detail::reject_next_spawn_for_test.store(
            true, std::memory_order_release);
        group.spawn([]() -> task<void> { co_return; });

        try {
            co_await group.join();
        } catch (const std::logic_error& error) {
            rejection_propagated.store(
                std::string_view(error.what()).find("scheduler rejected") !=
                    std::string_view::npos,
                std::memory_order_release);
        }
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(callback_reentered.load(std::memory_order_acquire));
    REQUIRE(rejection_propagated.load(std::memory_order_acquire));
    REQUIRE_FALSE(
        elio::runtime::detail::reject_next_spawn_for_test.load(
            std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("task_group join covers scheduler-owned callable lifetime",
          "[task_group][structured][lifetime]") {
    scheduler sched(1);
    sched.start();
    std::atomic<bool> callable_destroyed{false};
    std::atomic<bool> join_observed_destruction{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        task_group group;
        group.spawn(lifetime_probe(callable_destroyed));
        co_await group.join();
        join_observed_destruction.store(
            callable_destroyed.load(std::memory_order_acquire),
            std::memory_order_release);
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(join_observed_destruction.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("task_group does not start children after cancellation",
          "[task_group][structured][cancellation]") {
    scheduler sched(1);
    sched.start();
    std::atomic<bool> child_invoked{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        task_group group;
        group.request_cancel();
        group.spawn([&]() -> task<void> {
            child_invoked.store(true, std::memory_order_release);
            co_return;
        });
        co_await group.join();
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE_FALSE(child_invoked.load(std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("task_group rejects a join from another scheduler domain",
          "[task_group][structured][contract][lifetime]") {
    scheduler owner_scheduler(1);
    scheduler foreign_scheduler(1);
    owner_scheduler.start();
    foreign_scheduler.start();
    elio::sync::event release_child;
    task_group group(owner_scheduler);
    group.spawn([&]() -> task<void> {
        co_await release_child.wait();
    });
    std::atomic<bool> foreign_rejected{false};

    auto foreign = foreign_scheduler.go_joinable([&]() -> task<void> {
        try {
            co_await group.join();
        } catch (const std::logic_error&) {
            foreign_rejected.store(true, std::memory_order_release);
        }
    });
    foreign.wait_destroyed();
    REQUIRE_NOTHROW(foreign.await_resume());
    REQUIRE(foreign_rejected.load(std::memory_order_acquire));

    std::atomic<bool> owner_join_entering{false};
    auto owner = owner_scheduler.go_joinable([&]() -> task<void> {
        owner_join_entering.store(true, std::memory_order_release);
        co_await group.join();
    });
    REQUIRE(wait_for_flag(owner_join_entering));
    release_child.set();
    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());

    foreign_scheduler.shutdown();
    owner_scheduler.shutdown();
}

TEST_CASE("task_group cancellation from another scheduler is non-blocking",
          "[task_group][structured][cancellation][scheduler_domain]") {
    scheduler owner_scheduler(1);
    scheduler foreign_scheduler(1);
    owner_scheduler.start();
    foreign_scheduler.start();
    elio::sync::event never;
    std::atomic<bool> child_started{false};
    std::atomic<bool> release_owner_worker{false};
    std::atomic<bool> cancellation_returned{false};
    std::atomic<bool> child_cancelled{false};

    task_group group(owner_scheduler);
    group.spawn([&]() -> task<void> {
        child_started.store(true, std::memory_order_release);
        while (!release_owner_worker.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const auto result = co_await never.wait(
            elio::coro::this_coro::cancel_token());
        child_cancelled.store(
            result == cancel_result::cancelled, std::memory_order_release);
    });

    REQUIRE(wait_for_flag(child_started));
    auto foreign = foreign_scheduler.go_joinable([&]() -> task<void> {
        group.request_cancel();
        cancellation_returned.store(true, std::memory_order_release);
        co_return;
    });

    const bool returned_before_owner_released =
        wait_for_flag(cancellation_returned);
    release_owner_worker.store(true, std::memory_order_release);
    foreign.wait_destroyed();
    REQUIRE_NOTHROW(foreign.await_resume());

    auto owner = owner_scheduler.go_joinable([&]() -> task<void> {
        co_await group.join();
    });
    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());

    REQUIRE(returned_before_owner_released);
    REQUIRE(child_cancelled.load(std::memory_order_acquire));
    foreign_scheduler.shutdown();
    owner_scheduler.shutdown();
}

TEST_CASE("task_group cancellation dispatches from the scheduler start caller",
          "[task_group][structured][cancellation][scheduler_domain]") {
    scheduler sched(1);
    sched.start();
    elio::sync::event never;
    std::atomic<bool> child_started{false};
    std::atomic<bool> callback_on_worker{false};

    task_group group(sched);
    group.spawn([&]() -> task<void> {
        auto token = elio::coro::this_coro::cancel_token();
        auto registration = token.on_cancel([&] {
            callback_on_worker.store(
                scheduler::current() == &sched &&
                    elio::runtime::worker_thread::current() != nullptr,
                std::memory_order_release);
        });
        child_started.store(true, std::memory_order_release);
        (void)co_await never.wait(std::move(token));
    });

    REQUIRE(wait_for_flag(child_started));
    REQUIRE(scheduler::current() == &sched);
    REQUIRE(elio::runtime::worker_thread::current() == nullptr);
    group.request_cancel();
    REQUIRE(callback_on_worker.load(std::memory_order_acquire));

    auto owner = sched.go_joinable([&]() -> task<void> {
        co_await group.join();
    });
    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    sched.shutdown();
}

TEST_CASE("task_group join survives one completion enqueue rejection",
          "[task_group][structured][scheduler][failure]") {
    scheduler sched(1);
    sched.start();
    std::atomic<bool> join_completed{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        task_group group;
        group.spawn([]() -> task<void> {
            elio::runtime::detail::reject_next_schedule_for_test.store(
                true, std::memory_order_release);
            co_return;
        });
        co_await group.join();
        join_completed.store(true, std::memory_order_release);
    });

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(join_completed.load(std::memory_order_acquire));
    REQUIRE_FALSE(
        elio::runtime::detail::reject_next_schedule_for_test.load(
            std::memory_order_acquire));
    sched.shutdown();
}

TEST_CASE("task_group publishes completion after child state release",
          "[task_group][structured][lifetime][exception][regression]") {
    scheduler sched(2);
    sched.start();
    struct completion_pause_guard {
        completion_pause_guard() {
            elio::coro::detail::task_group_completion_paused_for_test.store(
                false, std::memory_order_release);
            elio::coro::detail::task_group_join_observed_pending_for_test.store(
                false, std::memory_order_release);
            elio::coro::detail::pause_before_task_group_completion_for_test.store(
                true, std::memory_order_release);
        }

        ~completion_pause_guard() { release(); }

        void release() const noexcept {
            elio::coro::detail::pause_before_task_group_completion_for_test.store(
                false, std::memory_order_release);
            elio::coro::detail::pause_before_task_group_completion_for_test
                .notify_all();
        }
    } pause_guard;
    elio::sync::event enter_join;
    elio::sync::event allow_child_failure;
    std::atomic<bool> child_running_on_worker_1{false};
    std::atomic<bool> owner_running_on_worker_0{false};
    std::atomic<bool> failure_observed{false};

    auto owner = sched.go_joinable_to(0, [&]() -> task<void> {
        co_await elio::set_affinity(0);
        owner_running_on_worker_0.store(
            elio::current_worker_id() == 0, std::memory_order_release);
        task_group group;
        group.spawn([&]() -> task<void> {
            co_await elio::set_affinity(1);
            child_running_on_worker_1.store(
                elio::current_worker_id() == 1, std::memory_order_release);
            co_await allow_child_failure.wait();
            throw std::runtime_error("child failure");
            co_return;
        });

        co_await enter_join.wait();
        try {
            co_await group.join();
        } catch (const std::runtime_error&) {
            failure_observed.store(true, std::memory_order_release);
        }
    });

    const bool child_running = wait_for_flag(child_running_on_worker_1);
    allow_child_failure.set();
    const bool completion_paused = wait_for_flag(
        elio::coro::detail::task_group_completion_paused_for_test);
    enter_join.set();
    const bool join_observed_pending = wait_for_flag(
        elio::coro::detail::task_group_join_observed_pending_for_test);
    CHECK_FALSE(owner.is_ready());

    pause_guard.release();
    const bool owner_destroyed = wait_for_condition([&] {
        return owner.is_destroyed();
    });
    const bool shutdown_ok = sched.shutdown(std::chrono::seconds(5));

    REQUIRE(child_running);
    REQUIRE(owner_running_on_worker_0);
    REQUIRE(completion_paused);
    REQUIRE(join_observed_pending);
    REQUIRE(owner_destroyed);
    REQUIRE(shutdown_ok);
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(failure_observed.load(std::memory_order_acquire));
}
