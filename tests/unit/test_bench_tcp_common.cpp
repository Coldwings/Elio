#include <catch2/catch_test_macros.hpp>

#include "../../examples/bench_tcp_common.hpp"

TEST_CASE("TCP ping-pong watchdog budget includes stall grace",
          "[bench][tcp]") {
    bench::config cfg;
    cfg.warmup_s = 2;
    cfg.duration_s = 5;

    CHECK(bench::pingpong_watchdog_budget(cfg) ==
          std::chrono::seconds(12));

    cfg.warmup_s = 3;
    cfg.duration_s = 10;

    CHECK(bench::pingpong_watchdog_budget(cfg) ==
          std::chrono::seconds(23));
}
