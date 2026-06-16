#include <catch2/catch_test_macros.hpp>
#include <elio/sync/channel.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <chrono>
#include <thread>

using namespace elio;
using namespace elio::sync;
using namespace elio::coro;
using namespace elio::runtime;

TEST_CASE("close wakes blocked sender", "[sync][channel][coro]") {
    channel<int> ch(1);
    ch.try_send(100);  // fills the buffer

    std::atomic<bool> sender_done{false};
    channel<int>* ch_ptr = &ch;
    std::atomic<bool>* sd_ptr = &sender_done;

    auto blocked_sender = [=]() -> task<void> {
        co_await ch_ptr->send(200);
        *sd_ptr = true;
        co_return;
    };

    scheduler sched(2);
    sched.start();

    auto s_join = sched.go_joinable(blocked_sender);

    // Give the sender time to block on send_waiters_
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ch.close();

    s_join.wait_destroyed();

    sched.shutdown();

    REQUIRE(sender_done);
}
