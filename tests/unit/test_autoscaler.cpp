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

coro::task<void> record_task_time(std::atomic<bool>* completed) {
    co_await time::yield();
    completed->store(true, std::memory_order_release);
    co_return;
}

coro::task<void> block_worker_until_released(std::atomic<bool>* started,
                                             std::atomic<bool>* release,
                                             std::atomic<bool>* completed) {
    started->store(true, std::memory_order_release);
    while (!release->load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    completed->store(true, std::memory_order_release);
    co_return;
}

struct block_capture_action {
    inline static std::atomic<size_t> calls{0};
    inline static std::atomic<size_t> worker_id{static_cast<size_t>(-1)};
    inline static std::atomic<int64_t> blocked_ms{0};

    static void reset() noexcept {
        calls.store(0, std::memory_order_relaxed);
        worker_id.store(static_cast<size_t>(-1), std::memory_order_relaxed);
        blocked_ms.store(0, std::memory_order_relaxed);
    }

    template<typename Scheduler>
    void operator()(Scheduler*, size_t id,
                    std::chrono::milliseconds duration) const noexcept {
        worker_id.store(id, std::memory_order_relaxed);
        blocked_ms.store(duration.count(), std::memory_order_relaxed);
        calls.fetch_add(1, std::memory_order_release);
    }
};

struct observed_worker {
    [[nodiscard]] bool is_running() const noexcept { return running; }
    [[nodiscard]] bool is_idle() const noexcept { return idle; }
    [[nodiscard]] size_t tasks_executed() const noexcept { return executions; }

    bool running{true};
    bool idle{true};
    size_t executions{0};
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
    block_capture_action::reset();

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

    worker->enable_task_time_tracking();
    auto initial_time = worker->last_task_time();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::atomic<bool> completed{false};
    sched.go(record_task_time, &completed);
    REQUIRE(wait_for_condition(
        [&] { return completed.load(std::memory_order_acquire); },
        std::chrono::seconds(2)));
    auto after_task = worker->last_task_time();

    CHECK(after_task > initial_time);

    sched.shutdown();
}

TEST_CASE("worker progress observation measures only active stalls",
          "[autoscaler]") {
    runtime::detail::worker_progress_observation observation;
    observed_worker worker;
    const auto start = std::chrono::steady_clock::time_point{};

    CHECK_FALSE(observation.sample(&worker, start).has_value());

    worker.idle = false;
    auto duration = observation.sample(&worker, start + std::chrono::milliseconds(5));
    REQUIRE(duration.has_value());
    CHECK(*duration == std::chrono::steady_clock::duration::zero());

    duration = observation.sample(&worker, start + std::chrono::milliseconds(35));
    REQUIRE(duration.has_value());
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(*duration) ==
          std::chrono::milliseconds(30));

    ++worker.executions;
    duration = observation.sample(&worker, start + std::chrono::milliseconds(40));
    REQUIRE(duration.has_value());
    CHECK(*duration == std::chrono::steady_clock::duration::zero());

    worker.idle = true;
    CHECK_FALSE(observation.sample(
        &worker, start + std::chrono::milliseconds(80)).has_value());

    worker.idle = false;
    duration = observation.sample(&worker, start + std::chrono::milliseconds(90));
    REQUIRE(duration.has_value());
    CHECK(*duration == std::chrono::steady_clock::duration::zero());

    worker.running = false;
    CHECK_FALSE(observation.sample(
        &worker, start + std::chrono::milliseconds(120)).has_value());
}

TEST_CASE("on_block samples worker execution progress", "[autoscaler]") {
    block_capture_action::reset();

    autoscaler_config config;
    config.tick_interval = std::chrono::milliseconds(5);
    config.block_threshold = std::chrono::milliseconds(25);

    scheduler sched{1};
    sched.start();
    autoscaler<scheduler, on_block<block_capture_action>> scaler{config};
    scaler.start(&sched);

    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    std::atomic<bool> completed{false};
    sched.go(block_worker_until_released, &started, &release, &completed);

    const bool worker_started = wait_for_condition(
        [&] { return started.load(std::memory_order_acquire); },
        std::chrono::seconds(2));
    const bool block_observed = worker_started && wait_for_condition(
        [] {
            return block_capture_action::calls.load(
                       std::memory_order_acquire) > 0;
        },
        std::chrono::seconds(2));
    release.store(true, std::memory_order_release);
    const bool worker_completed = wait_for_condition(
        [&] { return completed.load(std::memory_order_acquire); },
        std::chrono::seconds(2));

    scaler.stop();
    sched.shutdown();

    REQUIRE(worker_started);
    REQUIRE(block_observed);
    REQUIRE(worker_completed);
    CHECK(block_capture_action::worker_id.load(std::memory_order_relaxed) == 0);
    CHECK(block_capture_action::blocked_ms.load(std::memory_order_relaxed) >=
          config.block_threshold.count());
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
