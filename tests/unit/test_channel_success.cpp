/// Tests for channel send() success_ flag correctness.
///
/// Verifies that send() returns true when a receiver directly steals
/// the value from a blocked sender (the bug fixed in issue #234).
///
/// Regression tests for: send() must report success when its value is
/// delivered to a receiver via a direct handoff (the receiver steals from
/// a blocked sender), even if the channel is closed before the sender
/// resumes. Without setting sender->success_ = true in the steal paths,
/// send() falls back to `!closed_` and incorrectly returns false once
/// the channel is closed.
///
/// To make the "close before the sender resumes" window deterministic,
/// a single-threaded scheduler is used and the steal + close are
/// performed inside one coroutine: schedule_handle() enqueues the
/// resumed sender onto the only worker, so it cannot run until the
/// stealer coroutine yields, guaranteeing close() wins the race.

#include <catch2/catch_test_macros.hpp>
#include <elio/sync/channel.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>

#include <atomic>
#include <chrono>
#include <thread>

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
// ---------------------------------------------------------------------------
TEST_CASE("channel send returns true after recv() bounded direct steal",
          "[sync][channel][coro]") {
    channel<int> ch(1);   // bounded(1)
    ch.try_send(1);       // fill the ring so the next sender blocks

    std::atomic<bool> send_result{false};
    std::atomic<bool> sender_done{false};
    std::atomic<int> stolen{-1};

    channel<int>* ch_ptr = &ch;
    std::atomic<bool>* sr_ptr = &send_result;
    std::atomic<bool>* sd_ptr = &sender_done;
    std::atomic<int>* st_ptr = &stolen;

    auto blocked_sender = [=]() -> task<void> {
        bool r = co_await ch_ptr->send(2);  // blocks: ring full
        *sr_ptr = r;
        *sd_ptr = true;
        co_return;
    };

    scheduler sched(1);  // single worker for deterministic ordering
    sched.start();
    auto s_join = spawn_joinable(sched, blocked_sender);

    // Let the sender reach await_suspend and enqueue in send_waiters_.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto stealer = [=]() -> task<void> {
        auto v0 = ch_ptr->try_recv();      // pop buffered 1 (ring now empty)
        (void)v0;
        auto v = co_await ch_ptr->recv();  // direct-steal 2 from blocked sender
        if (v) *st_ptr = *v;
        ch_ptr->close();                   // close before sender resumes
        co_return;
    };
    auto r_join = spawn_joinable(sched, stealer);

    r_join.wait_destroyed();
    s_join.wait_destroyed();
    sched.shutdown();

    REQUIRE(stolen.load() == 2);          // value was delivered to the receiver
    REQUIRE(sender_done.load());          // sender resumed
    REQUIRE(send_result.load());          // send() reported success
}

// ---------------------------------------------------------------------------
// Test 2: recv() direct steal from rendezvous sender
// ---------------------------------------------------------------------------
TEST_CASE("channel send returns true after recv() rendezvous direct steal",
          "[sync][channel][coro]") {
    channel<int> ch;  // rendezvous

    std::atomic<bool> send_result{false};
    std::atomic<bool> sender_done{false};
    std::atomic<int> stolen{-1};

    channel<int>* ch_ptr = &ch;
    std::atomic<bool>* sr_ptr = &send_result;
    std::atomic<bool>* sd_ptr = &sender_done;
    std::atomic<int>* st_ptr = &stolen;

    auto blocked_sender = [=]() -> task<void> {
        bool r = co_await ch_ptr->send(7);  // blocks: no receiver ready
        *sr_ptr = r;
        *sd_ptr = true;
        co_return;
    };

    scheduler sched(1);  // single worker for deterministic ordering
    sched.start();
    auto s_join = spawn_joinable(sched, blocked_sender);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto stealer = [=]() -> task<void> {
        auto v = co_await ch_ptr->recv();  // direct-steal 7 from rendezvous sender
        if (v) *st_ptr = *v;
        ch_ptr->close();                   // close before sender resumes
        co_return;
    };
    auto r_join = spawn_joinable(sched, stealer);

    r_join.wait_destroyed();
    s_join.wait_destroyed();
    sched.shutdown();

    REQUIRE(stolen.load() == 7);
    REQUIRE(sender_done.load());
    REQUIRE(send_result.load());
}

// ---------------------------------------------------------------------------
// Test 3: try_recv() bounded ring push wakes blocked sender
// ---------------------------------------------------------------------------
TEST_CASE("channel send returns true after try_recv() bounded ring push",
          "[sync][channel][coro]") {
    channel<int> ch(1);   // bounded(1)
    ch.try_send(10);      // fill the ring so the next sender blocks

    std::atomic<bool> send_result{false};
    std::atomic<bool> sender_done{false};

    channel<int>* ch_ptr = &ch;
    std::atomic<bool>* sr_ptr = &send_result;
    std::atomic<bool>* sd_ptr = &sender_done;

    auto blocked_sender = [=]() -> task<void> {
        bool r = co_await ch_ptr->send(20);  // blocks: ring full
        *sr_ptr = r;
        *sd_ptr = true;
        co_return;
    };

    scheduler sched(2);
    sched.start();
    auto s_join = spawn_joinable(sched, blocked_sender);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // try_recv() pops 10, then pushes the blocked sender's 20 into the ring and
    // schedules the sender. close() runs from the main thread before the worker
    // resumes the sender, exercising the close-before-resume window.
    auto v1 = ch.try_recv();
    REQUIRE(v1.has_value());
    REQUIRE(*v1 == 10);

    ch.close();

    s_join.wait_destroyed();
    sched.shutdown();

    // The sender's value (20) was made available via the ring push.
    auto v2 = ch.try_recv();
    REQUIRE(v2.has_value());
    REQUIRE(*v2 == 20);
    REQUIRE(sender_done.load());
    REQUIRE(send_result.load());
}

// ---------------------------------------------------------------------------
// Test 4: try_recv() direct steal from rendezvous sender
// ---------------------------------------------------------------------------
TEST_CASE("channel send returns true after try_recv() rendezvous direct steal",
          "[sync][channel][coro]") {
    channel<int> ch;  // rendezvous

    std::atomic<bool> send_result{false};
    std::atomic<bool> sender_done{false};

    channel<int>* ch_ptr = &ch;
    std::atomic<bool>* sr_ptr = &send_result;
    std::atomic<bool>* sd_ptr = &sender_done;

    auto blocked_sender = [=]() -> task<void> {
        bool r = co_await ch_ptr->send(99);  // blocks: no receiver ready
        *sr_ptr = r;
        *sd_ptr = true;
        co_return;
    };

    scheduler sched(2);
    sched.start();
    auto s_join = spawn_joinable(sched, blocked_sender);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // try_recv() steals 99 directly from the rendezvous sender and schedules it;
    // close() runs before the worker resumes the sender.
    auto v = ch.try_recv();
    REQUIRE(v.has_value());
    REQUIRE(*v == 99);

    ch.close();

    s_join.wait_destroyed();
    sched.shutdown();

    REQUIRE(sender_done.load());
    REQUIRE(send_result.load());
}
