#include <catch2/catch_test_macros.hpp>
#include <elio/sync/channel.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>

using namespace elio::sync;
using namespace elio::coro;
using namespace elio::runtime;

TEST_CASE("simple channel send recv", "[sync][channel][coro]") {
    channel<int> ch(2);
    std::atomic<int> result{0};

    auto producer = [&]() -> task<void> {
        co_await ch.send(42);
        co_return;
    };

    auto consumer = [&]() -> task<void> {
        auto val = co_await ch.recv();
        if (val) {
            result = *val;
        }
        co_return;
    };

    scheduler sched(2);
    sched.start();

    auto producer_join = sched.go_joinable(producer);
    auto consumer_join = sched.go_joinable(consumer);

    producer_join.wait_destroyed();
    consumer_join.wait_destroyed();

    sched.shutdown();

    REQUIRE(result == 42);
}
