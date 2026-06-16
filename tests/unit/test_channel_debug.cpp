#include <catch2/catch_test_macros.hpp>
#include <elio/sync/channel.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <iostream>

using namespace elio;
using namespace elio::sync;
using namespace elio::coro;
using namespace elio::runtime;

TEST_CASE("channel debug", "[sync][channel][coro]") {
    channel<int> ch(2);
    std::atomic<int> sum{0};
    
    channel<int>* ch_ptr = &ch;
    std::atomic<int>* sum_ptr = &sum;
    
    auto producer = [=]() -> task<void> {
        std::cout << "Producer: starting" << std::endl;
        for (int i = 1; i <= 3; ++i) {
            std::cout << "Producer: sending " << i << std::endl;
            co_await ch_ptr->send(i);
            std::cout << "Producer: sent " << i << std::endl;
        }
        std::cout << "Producer: closing" << std::endl;
        ch_ptr->close();
        std::cout << "Producer: done" << std::endl;
        co_return;
    };
    
    auto consumer = [=]() -> task<void> {
        std::cout << "Consumer: starting" << std::endl;
        while (true) {
            std::cout << "Consumer: receiving (closed=" << ch_ptr->is_closed() << ")" << std::endl;
            auto val = co_await ch_ptr->recv();
            std::cout << "Consumer: recv returned, has_value=" << val.has_value() << std::endl;
            if (!val) {
                std::cout << "Consumer: channel closed" << std::endl;
                break;
            }
            std::cout << "Consumer: received " << *val << std::endl;
            *sum_ptr += *val;
        }
        std::cout << "Consumer: done" << std::endl;
        co_return;
    };
    
    scheduler sched(2);
    sched.start();
    
    std::cout << "Main: spawning producer and consumer" << std::endl;
    auto producer_join = sched.go_joinable(producer);
    auto consumer_join = sched.go_joinable(consumer);
    
    std::cout << "Main: waiting for producer" << std::endl;
    producer_join.wait_destroyed();
    std::cout << "Main: waiting for consumer" << std::endl;
    consumer_join.wait_destroyed();
    
    sched.shutdown();
    
    REQUIRE(sum == 6);  // 1+2+3
}

TEST_CASE("simple channel coro test", "[sync][channel][coro]") {
    channel<int> ch(2);
    
    auto producer = [&]() -> task<void> {
        bool ok = co_await ch.send(1);
        REQUIRE(ok);
        co_return;
    };
    
    auto consumer = [&]() -> task<void> {
        auto val = co_await ch.recv();
        REQUIRE(val.has_value());
        REQUIRE(*val == 1);
        co_return;
    };
    
    scheduler sched(2);
    sched.start();
    
    auto p_join = sched.go_joinable(producer);
    auto c_join = sched.go_joinable(consumer);
    
    p_join.wait_destroyed();
    c_join.wait_destroyed();
    
    sched.shutdown();
}
