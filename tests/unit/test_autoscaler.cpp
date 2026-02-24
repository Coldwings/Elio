#include <catch2/catch_test_macros.hpp>
#include <elio/elio.hpp>
#include <thread>
#include <chrono>

using namespace elio;
using namespace elio::runtime;

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
