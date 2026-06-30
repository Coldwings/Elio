/// Tests for channel send() success_ flag correctness.
///
/// Verifies that send() returns true when a receiver directly steals
/// the value from a blocked sender (the bug fixed in issue #234).

#include <catch2/catch_test_macros.hpp>
#include <elio/sync/channel.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>

#include <atomic>
#include <latch>
#include <vector>

using namespace elio::sync;
using namespace elio::coro;
using namespace elio::runtime;

// Helper to spawn a joinable task
template<typename F>
auto spawn_joinable(scheduler& sched, F&& f) {
    return sched.go_joinable(std::forward<F>(f));
}

// ---------------------------------------------------------------------------
// Test 1: recv() direct steal from blocked sender on bounded channel
//
// Fill a bounded channel to capacity so the next send() blocks.
// Then recv() pops from the ring and back-fills from the blocked sender.
// The blocked sender's send() must return true.
// ---------------------------------------------------------------------------
TEST_CASE("channel send returns true when recv steals from blocked sender (bounded)",
          "[sync][channel][coro]") {
    channel<int> ch(1);  // capacity 1
    ch.try_send(100);    // fill the buffer

    std::atomic<bool> send_result{false};
    std::atomic<bool> send_done{false};
    std::atomic<int> received_value{0};

    channel<int>* ch_ptr = &ch;
    auto* sr = &send_result;
    auto* sd = &send_done;
    auto* rv = &received_value;

    // This sender will block because the channel is full
    auto sender = [=]() -> task<void> {
        bool ok = co_await ch_ptr->send(200);
        sr->store(ok, std::memory_order_release);
        sd->store(true, std::memory_order_release);
        co_return;
    };

    // Receiver: pop from ring (gets 100), which triggers back-fill from
    // the blocked sender (200). Then recv again to get 200.
    auto receiver = [=]() -> task<void> {
        // First recv gets the buffered value
        auto v1 = co_await ch_ptr->recv();
        REQUIRE(v1.has_value());
        // Second recv gets the sender's value
        auto v2 = co_await ch_ptr->recv();
        if (v2.has_value()) {
            rv->store(*v2, std::memory_order_release);
        }
        co_return;
    };

    scheduler sched(2);
    sched.start();

    auto s_join = spawn_joinable(sched, sender);
    auto r_join = spawn_joinable(sched, receiver);

    s_join.wait_destroyed();
    r_join.wait_destroyed();

    sched.shutdown();

    // The critical assertion: send() must return true because the value
    // was successfully delivered to the receiver
    REQUIRE(send_result.load(std::memory_order_acquire) == true);
    REQUIRE(send_done.load(std::memory_order_acquire));
    REQUIRE(received_value.load(std::memory_order_acquire) == 200);
}

// ---------------------------------------------------------------------------
// Test 2: recv() direct steal from rendezvous sender
//
// On a rendezvous channel (capacity 0), send() blocks until a receiver
// arrives. When recv() directly steals from the blocked sender, the
// sender's send() must return true.
// ---------------------------------------------------------------------------
TEST_CASE("channel send returns true when recv steals from rendezvous sender",
          "[sync][channel][coro]") {
    channel<int> ch(0);  // rendezvous

    std::atomic<bool> send_result{false};
    std::atomic<bool> send_done{false};
    std::atomic<int> received_value{0};

    channel<int>* ch_ptr = &ch;
    auto* sr = &send_result;
    auto* sd = &send_done;
    auto* rv = &received_value;

    auto sender = [=]() -> task<void> {
        bool ok = co_await ch_ptr->send(42);
        sr->store(ok, std::memory_order_release);
        sd->store(true, std::memory_order_release);
        co_return;
    };

    auto receiver = [=]() -> task<void> {
        auto v = co_await ch_ptr->recv();
        if (v.has_value()) {
            rv->store(*v, std::memory_order_release);
        }
        co_return;
    };

    scheduler sched(2);
    sched.start();

    auto s_join = spawn_joinable(sched, sender);
    auto r_join = spawn_joinable(sched, receiver);

    s_join.wait_destroyed();
    r_join.wait_destroyed();

    sched.shutdown();

    REQUIRE(send_result.load(std::memory_order_acquire) == true);
    REQUIRE(send_done.load(std::memory_order_acquire));
    REQUIRE(received_value.load(std::memory_order_acquire) == 42);
}

// ---------------------------------------------------------------------------
// Test 3: try_recv() wakes blocked sender on bounded channel
//
// Fill a bounded channel, then have a sender block. A synchronous
// try_recv() pops from the ring and back-fills from the blocked sender.
// The sender's send() must return true.
// ---------------------------------------------------------------------------
TEST_CASE("channel send returns true when try_recv wakes blocked sender (bounded)",
          "[sync][channel][coro]") {
    channel<int> ch(1);  // capacity 1
    ch.try_send(100);    // fill the buffer

    std::atomic<bool> send_result{false};
    std::atomic<bool> send_done{false};

    channel<int>* ch_ptr = &ch;
    auto* sr = &send_result;
    auto* sd = &send_done;

    // This sender will block because the channel is full
    auto sender = [=]() -> task<void> {
        bool ok = co_await ch_ptr->send(200);
        sr->store(ok, std::memory_order_release);
        sd->store(true, std::memory_order_release);
        co_return;
    };

    scheduler sched(1);
    sched.start();

    auto s_join = spawn_joinable(sched, sender);

    // Give the sender time to suspend on the full channel
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // try_recv pops 100 from ring, then back-fills 200 from blocked sender
    auto v1 = ch.try_recv();
    REQUIRE(v1.has_value());
    REQUIRE(*v1 == 100);

    // Wait for sender to complete
    s_join.wait_destroyed();

    sched.shutdown();

    // The sender's value was pushed into the ring by try_recv, so send()
    // must return true
    REQUIRE(send_result.load(std::memory_order_acquire) == true);
    REQUIRE(send_done.load(std::memory_order_acquire));

    // Verify the back-filled value is now in the ring
    auto v2 = ch.try_recv();
    REQUIRE(v2.has_value());
    REQUIRE(*v2 == 200);
}

// ---------------------------------------------------------------------------
// Test 4: try_recv() direct steal from rendezvous sender
//
// On a rendezvous channel, a sender blocks. A synchronous try_recv()
// directly steals the value. The sender's send() must return true.
// ---------------------------------------------------------------------------
TEST_CASE("channel send returns true when try_recv steals from rendezvous sender",
          "[sync][channel][coro]") {
    channel<int> ch(0);  // rendezvous

    std::atomic<bool> send_result{false};
    std::atomic<bool> send_done{false};

    channel<int>* ch_ptr = &ch;
    auto* sr = &send_result;
    auto* sd = &send_done;

    auto sender = [=]() -> task<void> {
        bool ok = co_await ch_ptr->send(99);
        sr->store(ok, std::memory_order_release);
        sd->store(true, std::memory_order_release);
        co_return;
    };

    scheduler sched(1);
    sched.start();

    auto s_join = spawn_joinable(sched, sender);

    // Give the sender time to suspend
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // try_recv directly steals from the blocked rendezvous sender
    auto v = ch.try_recv();
    REQUIRE(v.has_value());
    REQUIRE(*v == 99);

    s_join.wait_destroyed();

    sched.shutdown();

    REQUIRE(send_result.load(std::memory_order_acquire) == true);
    REQUIRE(send_done.load(std::memory_order_acquire));
}

// ---------------------------------------------------------------------------
// Test 5: Multiple sends with interleaved recv (stress the success_ flag)
//
// Send multiple values through a small bounded channel where some sends
// will block and be woken by recv. All send() calls should return true.
// ---------------------------------------------------------------------------
TEST_CASE("channel multiple blocked sends all return true",
          "[sync][channel][coro]") {
    constexpr int N = 10;
    channel<int> ch(1);  // very small buffer to force blocking

    std::atomic<int> success_count{0};
    std::atomic<int> send_done_count{0};
    std::atomic<int> recv_sum{0};
    std::atomic<bool> all_received{false};

    channel<int>* ch_ptr = &ch;
    auto* sc = &success_count;
    auto* sdc = &send_done_count;
    auto* rs = &recv_sum;
    auto* ar = &all_received;

    auto receiver = [=]() -> task<void> {
        int sum = 0;
        for (int i = 0; i < N; ++i) {
            auto v = co_await ch_ptr->recv();
            if (v.has_value()) {
                sum += *v;
            }
        }
        rs->store(sum, std::memory_order_release);
        ar->store(true, std::memory_order_release);
        co_return;
    };

    scheduler sched(4);
    sched.start();

    // Spawn N senders and 1 receiver
    std::vector<join_handle<void>> joins;
    for (int i = 1; i <= N; ++i) {
        joins.push_back(spawn_joinable(sched, [=]() -> task<void> {
            bool ok = co_await ch_ptr->send(i);
            if (ok) sc->fetch_add(1, std::memory_order_relaxed);
            sdc->fetch_add(1, std::memory_order_release);
            co_return;
        }));
    }
    auto r_join = spawn_joinable(sched, receiver);

    for (auto& j : joins) {
        j.wait_destroyed();
    }
    r_join.wait_destroyed();

    sched.shutdown();

    // All N sends should have returned true
    REQUIRE(success_count.load(std::memory_order_acquire) == N);
    REQUIRE(send_done_count.load(std::memory_order_acquire) == N);
    REQUIRE(all_received.load(std::memory_order_acquire));
    // Sum of 1..10 = 55
    REQUIRE(recv_sum.load(std::memory_order_acquire) == 55);
}
