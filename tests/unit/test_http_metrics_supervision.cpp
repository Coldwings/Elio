#include <catch2/catch_test_macros.hpp>

#include "../integration/http_metrics_supervision.hpp"

#include <atomic>
#include <cstdint>

using elio::test::http_metrics::try_claim_expired_phase;

TEST_CASE("HTTP metrics phase expiry rejects a cleared snapshot",
          "[http][metrics][supervision][regression]") {
    std::atomic<int64_t> deadline{100};
    const auto sampled = deadline.load();
    deadline.store(0);

    REQUIRE_FALSE(try_claim_expired_phase(deadline, sampled, 101));
    REQUIRE(deadline.load() == 0);
}

TEST_CASE("HTTP metrics phase expiry rejects a replaced snapshot",
          "[http][metrics][supervision][regression]") {
    std::atomic<int64_t> deadline{100};
    const auto sampled = deadline.load();
    deadline.store(200);

    REQUIRE_FALSE(try_claim_expired_phase(deadline, sampled, 101));
    REQUIRE(deadline.load() == 200);
    // Even if the replacement has also expired, it must first be resampled.
    REQUIRE_FALSE(try_claim_expired_phase(deadline, sampled, 201));
    REQUIRE(deadline.load() == 200);
    REQUIRE(try_claim_expired_phase(deadline, deadline.load(), 201));
    REQUIRE(deadline.load() == 0);
}

TEST_CASE("HTTP metrics phase expiry claims an unchanged deadline once",
          "[http][metrics][supervision][regression]") {
    std::atomic<int64_t> deadline{100};
    const auto sampled = deadline.load();

    REQUIRE(try_claim_expired_phase(deadline, sampled, 100));
    REQUIRE(deadline.load() == 0);
    REQUIRE_FALSE(try_claim_expired_phase(deadline, sampled, 101));
}

TEST_CASE("HTTP metrics phase expiry leaves unexpired and disabled phases alone",
          "[http][metrics][supervision][regression]") {
    std::atomic<int64_t> deadline{100};

    REQUIRE_FALSE(try_claim_expired_phase(deadline, deadline.load(), 99));
    REQUIRE(deadline.load() == 100);
    deadline.store(0);
    REQUIRE_FALSE(try_claim_expired_phase(deadline, deadline.load(), 101));
    REQUIRE(deadline.load() == 0);
}

TEST_CASE("HTTP metrics phase expiry selection survives later worker progress",
          "[http][metrics][supervision][regression]") {
    std::atomic<int64_t> deadline{100};
    const bool expired = try_claim_expired_phase(deadline, deadline.load(), 100);
    deadline.store(0);
    deadline.store(200);

    REQUIRE(expired);
    REQUIRE(deadline.load() == 200);
}
