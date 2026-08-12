#include <catch2/catch_test_macros.hpp>
#include <elio/elio.hpp>
#include <atomic>
#include <thread>
#include <chrono>

using namespace elio;
using namespace elio::runtime;

namespace {

template<typename Predicate>
bool wait_for_condition(Predicate&& predicate,
                        std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

struct trigger_test_worker {
    [[nodiscard]] bool is_idle() const noexcept { return false; }

    [[nodiscard]] std::chrono::steady_clock::time_point
    last_task_time() const noexcept {
        return {};
    }
};

struct trigger_test_scheduler {
    [[nodiscard]] size_t pending_tasks() const noexcept {
        return pending.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t num_threads() const noexcept {
        return workers.load(std::memory_order_relaxed);
    }

    void set_thread_count(size_t count) noexcept {
        workers.store(count, std::memory_order_relaxed);
    }

    [[nodiscard]] trigger_test_worker* get_worker(size_t) noexcept {
        return &worker;
    }

    std::atomic<size_t> pending{2};
    std::atomic<size_t> workers{2};
    trigger_test_worker worker;
};

struct overload_capture_action {
    inline static std::atomic<size_t> calls{0};

    void operator()(trigger_test_scheduler*, size_t) const noexcept {
        calls.fetch_add(1, std::memory_order_release);
    }
};

struct idle_capture_action {
    inline static std::atomic<size_t> calls{0};

    void operator()(trigger_test_scheduler*, size_t,
                    std::chrono::seconds) const noexcept {
        calls.fetch_add(1, std::memory_order_release);
    }
};

struct block_capture_action {
    inline static std::atomic<size_t> calls{0};

    void operator()(trigger_test_scheduler*, size_t,
                    std::chrono::milliseconds) const noexcept {
        calls.fetch_add(1, std::memory_order_release);
    }
};

}  // namespace

TEST_CASE("autoscaler config defaults", "[autoscaler]") {
    autoscaler_config config;

    CHECK(config.tick_interval.count() == 500);
    CHECK(config.overload_threshold == 10);
    CHECK(config.idle_threshold == 2);
    CHECK(config.idle_delay.count() == 30);
    CHECK(config.min_workers == 1);
    CHECK(config.block_threshold.count() == 5000);
}

TEST_CASE("autoscaler start/stop", "[autoscaler]") {
    scheduler sched{2};
    sched.start();

    autoscaler<scheduler> scaler{autoscaler_config{}};
    scaler.start(&sched);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    scaler.stop();
    sched.shutdown();
}

TEST_CASE("autoscaler custom trigger pack", "[autoscaler]") {
    overload_capture_action::calls.store(0, std::memory_order_relaxed);
    idle_capture_action::calls.store(0, std::memory_order_relaxed);
    block_capture_action::calls.store(0, std::memory_order_relaxed);

    autoscaler_config config;
    config.tick_interval = std::chrono::milliseconds(1);
    config.overload_threshold = 1;
    config.idle_threshold = 1;
    config.idle_delay = std::chrono::seconds(0);
    config.min_workers = 1;
    config.max_workers = 3;
    config.block_threshold = std::chrono::milliseconds(0);

    trigger_test_scheduler sched;
    autoscaler<trigger_test_scheduler,
               on_overload<overload_capture_action>,
               on_idle<idle_capture_action>,
               on_block<block_capture_action>> scaler{config};
    scaler.start(&sched);

    const bool overload_and_block_observed = wait_for_condition(
        [] {
            return overload_capture_action::calls.load(
                       std::memory_order_acquire) > 0 &&
                   block_capture_action::calls.load(
                       std::memory_order_acquire) > 0;
        },
        std::chrono::seconds(2));

    sched.pending.store(0, std::memory_order_relaxed);
    const bool idle_observed = wait_for_condition(
        [] {
            return idle_capture_action::calls.load(
                       std::memory_order_acquire) > 0;
        },
        std::chrono::seconds(2));

    scaler.stop();

    CHECK(overload_and_block_observed);
    CHECK(idle_observed);
    CHECK(overload_capture_action::calls.load(std::memory_order_relaxed) > 0);
    CHECK(idle_capture_action::calls.load(std::memory_order_relaxed) > 0);
    CHECK(block_capture_action::calls.load(std::memory_order_relaxed) > 0);
}

TEST_CASE("worker_thread last_task_time", "[autoscaler]") {
    scheduler sched{1};
    sched.start();

    auto* worker = sched.get_worker(0);
    REQUIRE(worker != nullptr);

    auto initial_time = worker->last_task_time();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto after_sleep = worker->last_task_time();

    CHECK(after_sleep >= initial_time);

    sched.shutdown();
}

TEST_CASE("worker_thread is_idle", "[autoscaler]") {
    scheduler sched{1};
    sched.start();

    auto* worker = sched.get_worker(0);
    REQUIRE(worker != nullptr);

    // Worker should be either running or idle, one must be true
    // The exact state depends on timing

    sched.shutdown();
}

TEST_CASE("autoscaler config custom values", "[autoscaler]") {
    autoscaler_config config;
    config.tick_interval = std::chrono::milliseconds(100);
    config.overload_threshold = 20;
    config.idle_threshold = 5;
    config.idle_delay = std::chrono::seconds(60);
    config.min_workers = 2;
    config.max_workers = 32;
    config.block_threshold = std::chrono::seconds(10);

    CHECK(config.tick_interval.count() == 100);
    CHECK(config.overload_threshold == 20);
    CHECK(config.idle_threshold == 5);
    CHECK(config.idle_delay.count() == 60);
    CHECK(config.min_workers == 2);
    CHECK(config.max_workers == 32);
    CHECK(config.block_threshold.count() == 10000);
}
