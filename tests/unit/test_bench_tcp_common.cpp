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

TEST_CASE("TCP benchmark parser accepts documented modes", "[bench][tcp][args]") {
    auto parse_mode = [](const char* value) {
        const char* prog = "bench";
        const char* flag = "-m";
        char* argv[] = {
            const_cast<char*>(prog),
            const_cast<char*>(flag),
            const_cast<char*>(value),
        };
        return bench::parse_args(3, argv, "Test").type;
    };

    CHECK(parse_mode("pingpong") == bench::config::test_type::pingpong);
    CHECK(parse_mode("streaming") == bench::config::test_type::streaming);
    CHECK(parse_mode("both") == bench::config::test_type::both);
}

TEST_CASE("TCP benchmark parser rejects invalid modes", "[bench][tcp][args]") {
    const char* prog = "bench";
    const char* flag = "-m";
    const char* value = "pingpon";
    char* argv[] = {
        const_cast<char*>(prog),
        const_cast<char*>(flag),
        const_cast<char*>(value),
    };

    REQUIRE_THROWS_AS(bench::parse_args(3, argv, "Test"),
                      bench::argument_error);
}
