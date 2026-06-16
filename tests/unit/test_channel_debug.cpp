#include <catch2/catch_test_macros.hpp>
#include <elio/sync/channel.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>

using namespace elio::sync;
using namespace elio::coro;
using namespace elio::runtime;

TEST_CASE("channel with coroutines debug", "[sync][channel][coro]") {
    channel<int> ch(2);
    std::atomic<int> sum{0};
    std::atomic<bool> producer_done{false};
    std::atomic<bool> consumer_done{false};

    channel<int>* ch_ptr = &ch;
    std::atomic<int>* sum_ptr = &sum;
    std::atomic<bool>* producer_done_ptr = &producer_done;
    std::atomic<bool>* consumer_done_ptr = &consumer_done;

    auto producer = [=]() -> task<void> {
        for (int i = 1; i <= 5; ++i) {
            co_await ch_ptr->send(i);
        }
        ch_ptr->close();
        *producer_done_ptr = true;
        co_return;
    };

    auto consumer = [=]() -> task<void> {
        while (true) {
            auto val = co_await ch_ptr->recv();
            if (!val) {
                break;
            }
            *sum_ptr += *val;
        }
        *consumer_done_ptr = true;
        co_return;
    };

    scheduler sched(2);
    sched.start();

    auto producer_join = sched.go_joinable(producer);
    auto consumer_join = sched.go_joinable(consumer);

    producer_join.wait_destroyed();
    consumer_join.wait_destroyed();

    sched.shutdown();

    REQUIRE(*producer_done_ptr);
    REQUIRE(*consumer_done_ptr);
    REQUIRE(*sum_ptr == 15);
}
