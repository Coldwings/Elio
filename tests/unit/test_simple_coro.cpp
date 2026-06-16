#include <catch2/catch_test_macros.hpp>
#include <elio/sync/channel.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <iostream>

using namespace elio;
using namespace elio::sync;
using namespace elio::coro;
using namespace elio::runtime;

TEST_CASE("simple channel coro", "[sync][channel][coro]") {
    channel<int> ch(2);
    
    auto producer = [&]() -> task<void> {
        std::cout << "Producer: sending 1" << std::endl;
        bool ok = co_await ch.send(1);
        std::cout << "Producer: send returned " << ok << std::endl;
        co_return;
    };
    
    auto consumer = [&]() -> task<void> {
        std::cout << "Consumer: receiving" << std::endl;
        auto val = co_await ch.recv();
        std::cout << "Consumer: received " << (val ? std::to_string(*val) : "nullopt") << std::endl;
        co_return;
    };
    
    scheduler sched(2);
    sched.start();
    
    std::cout << "Main: spawning producer" << std::endl;
    auto p_join = sched.go_joinable(producer);
    
    std::cout << "Main: spawning consumer" << std::endl;
    auto c_join = sched.go_joinable(consumer);
    
    std::cout << "Main: waiting for producer" << std::endl;
    p_join.wait_destroyed();
    
    std::cout << "Main: waiting for consumer" << std::endl;
    c_join.wait_destroyed();
    
    sched.shutdown();
    
    std::cout << "Main: done" << std::endl;
    REQUIRE(true);
}
