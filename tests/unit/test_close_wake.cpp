#include <catch2/catch_test_macros.hpp>
#include <elio/sync/channel.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <iostream>
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
        std::cout << "Sender: about to send" << std::endl;
        bool result = co_await ch_ptr->send(200);
        std::cout << "Sender: send returned " << result << std::endl;
        *sd_ptr = true;
        std::cout << "Sender: done" << std::endl;
        co_return;
    };

    scheduler sched(2);
    sched.start();

    std::cout << "Main: spawning sender" << std::endl;
    auto s_join = sched.go_joinable(blocked_sender);

    std::cout << "Main: sleeping 50ms" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::cout << "Main: calling close()" << std::endl;
    ch.close();
    std::cout << "Main: close() returned" << std::endl;

    std::cout << "Main: waiting for sender" << std::endl;
    s_join.wait_destroyed();
    std::cout << "Main: sender done" << std::endl;

    sched.shutdown();

    REQUIRE(sender_done);
}
