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
#include <memory>
#include <optional>
#include <thread>

using namespace elio::sync;
using namespace elio::coro;
using namespace elio::runtime;

// Helper to spawn a joinable task
template<typename F>
auto spawn_joinable(scheduler& sched, F&& f) {
    return sched.go_joinable(std::forward<F>(f));
}

namespace {

struct slot_release_gate {
    std::atomic<bool> block_next_move{false};
    std::atomic<bool> move_blocked{false};
    std::atomic<bool> release_move{false};
};

struct gated_value {
    int value = 0;
    std::shared_ptr<slot_release_gate> gate;

    explicit gated_value(int v,
                         std::shared_ptr<slot_release_gate> g = {}) noexcept
        : value(v), gate(std::move(g)) {}

    gated_value(gated_value&& other) noexcept
        : value(other.value), gate(std::move(other.gate)) {
        maybe_block();
    }

    gated_value& operator=(gated_value&& other) noexcept {
        value = other.value;
        gate = std::move(other.gate);
        maybe_block();
        return *this;
    }

    gated_value(const gated_value&) = delete;
    gated_value& operator=(const gated_value&) = delete;

private:
    void maybe_block() noexcept {
        if (!gate || !gate->block_next_move.exchange(
                         false, std::memory_order_acq_rel)) {
            return;
        }
        gate->move_blocked.store(true, std::memory_order_release);
        gate->move_blocked.notify_all();
        while (!gate->release_move.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
};

bool wait_for_true(std::atomic<bool>& flag,
                   std::chrono::milliseconds timeout =
                       std::chrono::milliseconds(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flag.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return flag.load(std::memory_order_acquire);
}

bool wait_for_at_least(std::atomic<size_t>& value, size_t expected,
                       std::chrono::milliseconds timeout =
                           std::chrono::milliseconds(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (value.load(std::memory_order_acquire) < expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return value.load(std::memory_order_acquire) >= expected;
}

template<typename Predicate>
bool wait_for_condition(Predicate&& predicate,
                        std::chrono::milliseconds timeout =
                            std::chrono::milliseconds(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return predicate();
}

}  // namespace

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

TEST_CASE("bounded channel send waits for a logically freed slot to publish",
          "[sync][channel][coro][regression]") {
    auto run_case = [](bool token_aware) {
        auto gate = std::make_shared<slot_release_gate>();
        channel<gated_value> ch(2);
        REQUIRE(ch.try_send(gated_value(1, gate)));
        REQUIRE(ch.try_send(gated_value(2)));

        gate->block_next_move.store(true, std::memory_order_release);
        std::optional<gated_value> first;
        std::thread consumer([&] {
            first = ch.try_recv();
        });

        const bool consumer_claimed_slot = wait_for_true(gate->move_blocked);
        if (!consumer_claimed_slot) {
            gate->release_move.store(true, std::memory_order_release);
            consumer.join();
            REQUIRE(consumer_claimed_slot);
            return;
        }

        elio::sync::detail::bounded_send_publish_waits_for_test.store(
            0, std::memory_order_release);
        scheduler sched(1);
        sched.start();
        auto sender = sched.go_joinable([&]() -> task<bool> {
            if (!token_aware) {
                co_return co_await ch.send(gated_value(3));
            }

            cancel_source source;
            auto result = co_await ch.send(
                gated_value(3), source.get_token());
            co_return result.success() && !result.was_cancelled() &&
                      !result.was_closed();
        });

        const bool sender_observed_publish_window = wait_for_at_least(
            elio::sync::detail::bounded_send_publish_waits_for_test, 1);
        gate->release_move.store(true, std::memory_order_release);
        consumer.join();
        const bool sender_completed_without_cleanup = wait_for_condition(
            [&] { return sender.is_ready(); });
        if (!sender_completed_without_cleanup) {
            ch.close();
            const bool sender_completed_after_cleanup = wait_for_condition(
                [&] { return sender.is_ready(); });
            REQUIRE(sender_completed_after_cleanup);
        }
        REQUIRE(sender.is_ready());
        sender.wait_destroyed();
        const bool sent = sender.await_resume();
        REQUIRE(sched.shutdown(std::chrono::milliseconds(2000)));

        REQUIRE(sender_completed_without_cleanup);
        REQUIRE(consumer_claimed_slot);
        REQUIRE(sender_observed_publish_window);
        REQUIRE(first.has_value());
        REQUIRE(first->value == 1);
        REQUIRE(sent);

        auto second = ch.try_recv();
        auto third = ch.try_recv();
        REQUIRE(second.has_value());
        REQUIRE(second->value == 2);
        REQUIRE(third.has_value());
        REQUIRE(third->value == 3);
    };

    SECTION("no-token send") {
        run_case(false);
    }
    SECTION("token-aware send") {
        run_case(true);
    }
}

TEST_CASE("bounded channel drains refill credits after out-of-order publication",
          "[sync][channel][coro][regression]") {
    auto first_gate = std::make_shared<slot_release_gate>();
    auto second_gate = std::make_shared<slot_release_gate>();
    channel<gated_value> ch(2);
    REQUIRE(ch.try_send(gated_value(1, first_gate)));
    REQUIRE(ch.try_send(gated_value(2, second_gate)));

    first_gate->block_next_move.store(true, std::memory_order_release);
    second_gate->block_next_move.store(true, std::memory_order_release);
    std::optional<gated_value> first;
    std::optional<gated_value> second;
    std::atomic<bool> second_consumer_done{false};
    std::thread first_consumer([&] { first = ch.try_recv(); });
    const bool first_claimed = wait_for_true(first_gate->move_blocked);
    if (!first_claimed) {
        first_gate->release_move.store(true, std::memory_order_release);
        first_consumer.join();
        REQUIRE(first_claimed);
        return;
    }
    std::thread second_consumer([&] {
        second = ch.try_recv();
        second_consumer_done.store(true, std::memory_order_release);
    });
    const bool second_claimed = wait_for_true(second_gate->move_blocked);
    if (!second_claimed) {
        second_gate->release_move.store(true, std::memory_order_release);
        first_gate->release_move.store(true, std::memory_order_release);
        second_consumer.join();
        first_consumer.join();
        REQUIRE(second_claimed);
        return;
    }

    elio::sync::detail::bounded_send_publish_waits_for_test.store(
        0, std::memory_order_release);
    scheduler sched(2);
    sched.start();
    auto first_sender = sched.go_joinable([&]() -> task<bool> {
        co_return co_await ch.send(gated_value(3));
    });
    auto second_sender = sched.go_joinable([&]() -> task<bool> {
        co_return co_await ch.send(gated_value(4));
    });
    const bool both_senders_waited = wait_for_at_least(
        elio::sync::detail::bounded_send_publish_waits_for_test, 2);

    second_gate->release_move.store(true, std::memory_order_release);
    const bool second_completed_before_first = wait_for_true(
        second_consumer_done);
    first_gate->release_move.store(true, std::memory_order_release);
    second_consumer.join();
    first_consumer.join();

    const bool both_senders_completed_without_cleanup =
        wait_for_condition([&] {
            return first_sender.is_ready() && second_sender.is_ready();
        });
    if (!both_senders_completed_without_cleanup) {
        ch.close();
        const bool both_senders_completed_after_cleanup =
            wait_for_condition([&] {
                return first_sender.is_ready() && second_sender.is_ready();
            });
        REQUIRE(both_senders_completed_after_cleanup);
    }
    REQUIRE(first_sender.is_ready());
    REQUIRE(second_sender.is_ready());
    first_sender.wait_destroyed();
    second_sender.wait_destroyed();
    const bool first_sent = first_sender.await_resume();
    const bool second_sent = second_sender.await_resume();
    REQUIRE(sched.shutdown(std::chrono::milliseconds(2000)));

    REQUIRE(both_senders_completed_without_cleanup);
    REQUIRE(first_claimed);
    REQUIRE(second_claimed);
    REQUIRE(second_completed_before_first);
    REQUIRE(both_senders_waited);
    REQUIRE(first.has_value());
    REQUIRE(first->value == 1);
    REQUIRE(second.has_value());
    REQUIRE(second->value == 2);
    REQUIRE(first_sent);
    REQUIRE(second_sent);

    auto third = ch.try_recv();
    auto fourth = ch.try_recv();
    REQUIRE(third.has_value());
    REQUIRE(fourth.has_value());
    REQUIRE(((third->value == 3 && fourth->value == 4) ||
             (third->value == 4 && fourth->value == 3)));
}

TEST_CASE("bounded channel refill wakes a receiver queued behind consumers",
          "[sync][channel][coro][regression]") {
    auto first_gate = std::make_shared<slot_release_gate>();
    auto second_gate = std::make_shared<slot_release_gate>();
    channel<gated_value> ch(2);
    REQUIRE(ch.try_send(gated_value(1, first_gate)));
    REQUIRE(ch.try_send(gated_value(2, second_gate)));

    first_gate->block_next_move.store(true, std::memory_order_release);
    second_gate->block_next_move.store(true, std::memory_order_release);
    std::optional<gated_value> first;
    std::optional<gated_value> second;
    std::atomic<bool> second_consumer_done{false};
    std::thread first_consumer([&] { first = ch.try_recv(); });
    const bool first_claimed = wait_for_true(first_gate->move_blocked);
    if (!first_claimed) {
        first_gate->release_move.store(true, std::memory_order_release);
        first_consumer.join();
        REQUIRE(first_claimed);
        return;
    }
    std::thread second_consumer([&] {
        second = ch.try_recv();
        second_consumer_done.store(true, std::memory_order_release);
    });
    const bool second_claimed = wait_for_true(second_gate->move_blocked);
    if (!second_claimed) {
        second_gate->release_move.store(true, std::memory_order_release);
        first_gate->release_move.store(true, std::memory_order_release);
        second_consumer.join();
        first_consumer.join();
        REQUIRE(second_claimed);
        return;
    }

    elio::sync::detail::bounded_recv_waits_for_test.store(
        0, std::memory_order_release);
    scheduler sched(2);
    sched.start();
    auto receiver = sched.go_joinable([&]() -> task<int> {
        auto value = co_await ch.recv();
        co_return value ? value->value : -1;
    });
    const bool receiver_waited = wait_for_at_least(
        elio::sync::detail::bounded_recv_waits_for_test, 1);

    elio::sync::detail::bounded_send_publish_waits_for_test.store(
        0, std::memory_order_release);
    auto sender = sched.go_joinable([&]() -> task<bool> {
        co_return co_await ch.send(gated_value(3));
    });
    const bool sender_waited = wait_for_at_least(
        elio::sync::detail::bounded_send_publish_waits_for_test, 1);

    second_gate->release_move.store(true, std::memory_order_release);
    const bool second_completed_before_first = wait_for_true(
        second_consumer_done);
    first_gate->release_move.store(true, std::memory_order_release);
    second_consumer.join();
    first_consumer.join();

    const bool operations_completed_without_cleanup =
        wait_for_condition([&] {
            return receiver.is_ready() && sender.is_ready();
        });
    if (!operations_completed_without_cleanup) {
        ch.close();
        const bool operations_completed_after_cleanup =
            wait_for_condition([&] {
                return receiver.is_ready() && sender.is_ready();
            });
        REQUIRE(operations_completed_after_cleanup);
    }
    REQUIRE(receiver.is_ready());
    REQUIRE(sender.is_ready());
    receiver.wait_destroyed();
    sender.wait_destroyed();
    const int received = receiver.await_resume();
    const bool sent = sender.await_resume();
    REQUIRE(sched.shutdown(std::chrono::milliseconds(2000)));

    REQUIRE(operations_completed_without_cleanup);
    REQUIRE(first_claimed);
    REQUIRE(second_claimed);
    REQUIRE(second_completed_before_first);
    REQUIRE(receiver_waited);
    REQUIRE(sender_waited);
    REQUIRE(first.has_value());
    REQUIRE(first->value == 1);
    REQUIRE(second.has_value());
    REQUIRE(second->value == 2);
    REQUIRE(sent);
    REQUIRE(received == 3);
    REQUIRE(ch.empty());
}

TEST_CASE("bounded channel publication wait preserves cancellation winner",
          "[sync][channel][cancellation][regression]") {
    auto run_case = [](bool close_first) {
        auto gate = std::make_shared<slot_release_gate>();
        channel<gated_value> ch(2);
        REQUIRE(ch.try_send(gated_value(1, gate)));
        REQUIRE(ch.try_send(gated_value(2)));

        gate->block_next_move.store(true, std::memory_order_release);
        std::optional<gated_value> first;
        std::thread consumer([&] { first = ch.try_recv(); });
        const bool consumer_claimed = wait_for_true(gate->move_blocked);
        if (!consumer_claimed) {
            gate->release_move.store(true, std::memory_order_release);
            consumer.join();
            REQUIRE(consumer_claimed);
            return;
        }

        elio::sync::detail::bounded_send_publish_waits_for_test.store(
            0, std::memory_order_release);
        cancel_source source;
        scheduler sched(1);
        sched.start();
        auto sender = sched.go_joinable([&]()
                -> task<channel<gated_value>::cancellable_send_result> {
            co_return co_await ch.send(
                gated_value(3), source.get_token());
        });
        const bool sender_waited = wait_for_at_least(
            elio::sync::detail::bounded_send_publish_waits_for_test, 1);

        if (close_first) {
            ch.close();
            source.cancel();
        } else {
            source.cancel();
            ch.close();
        }

        bool sender_completed = wait_for_condition(
            [&] { return sender.is_ready(); });
        gate->release_move.store(true, std::memory_order_release);
        consumer.join();
        if (!sender_completed) {
            sender_completed = wait_for_condition(
                [&] { return sender.is_ready(); });
        }
        REQUIRE(sender_completed);
        sender.wait_destroyed();
        auto result = sender.await_resume();
        REQUIRE(sched.shutdown(std::chrono::milliseconds(2000)));

        REQUIRE(consumer_claimed);
        REQUIRE(sender_waited);
        REQUIRE(first.has_value());
        REQUIRE(first->value == 1);

        auto second = ch.try_recv();
        REQUIRE(second.has_value());
        REQUIRE(second->value == 2);
        auto third = ch.try_recv();
        if (close_first) {
            REQUIRE(result.success());
            REQUIRE_FALSE(result.was_cancelled());
            REQUIRE_FALSE(result.was_closed());
            REQUIRE(third.has_value());
            REQUIRE(third->value == 3);
        } else {
            REQUIRE_FALSE(result.success());
            REQUIRE(result.was_cancelled());
            REQUIRE_FALSE(result.was_closed());
            REQUIRE_FALSE(third.has_value());
        }
    };

    SECTION("cancellation wins before close") {
        run_case(false);
    }
    SECTION("close transfer wins before cancellation") {
        run_case(true);
    }
}

TEST_CASE("channel receive preserves notification against later cancellation",
          "[sync][channel][cancellation][regression]") {
    channel<int> ch(1);
    cancel_source source;
    elio::sync::detail::bounded_recv_waits_for_test.store(
        0, std::memory_order_release);

    scheduler sched(1);
    sched.start();
    auto first = sched.go_joinable([&]()
            -> task<channel<int>::cancellable_recv_result> {
        co_return co_await ch.recv(source.get_token());
    });
    const bool first_waited = wait_for_at_least(
        elio::sync::detail::bounded_recv_waits_for_test, 1);

    auto second = sched.go_joinable([&]() -> task<std::optional<int>> {
        co_return co_await ch.recv();
    });
    const bool second_waited = wait_for_at_least(
        elio::sync::detail::bounded_recv_waits_for_test, 2);

    std::atomic<bool> send_succeeded{false};
    auto notifier = sched.go_joinable([&]() -> task<void> {
        send_succeeded.store(ch.try_send(7), std::memory_order_release);
        source.cancel();
        co_return;
    });

    const bool notifier_completed = wait_for_condition(
        [&] { return notifier.is_ready(); });
    REQUIRE(notifier_completed);
    notifier.wait_destroyed();
    notifier.await_resume();

    const bool first_completed_without_cleanup = wait_for_condition(
        [&] { return first.is_ready(); });
    if (!first_completed_without_cleanup) {
        ch.close();
        const bool first_completed_after_cleanup = wait_for_condition(
            [&] { return first.is_ready(); });
        REQUIRE(first_completed_after_cleanup);
    }
    REQUIRE(first.is_ready());
    first.wait_destroyed();
    auto first_result = first.await_resume();

    ch.close();
    const bool second_completed = wait_for_condition(
        [&] { return second.is_ready(); });
    REQUIRE(second_completed);
    second.wait_destroyed();
    auto second_result = second.await_resume();
    REQUIRE(sched.shutdown(std::chrono::milliseconds(2000)));

    REQUIRE(first_completed_without_cleanup);
    REQUIRE(first_waited);
    REQUIRE(second_waited);
    REQUIRE(send_succeeded.load(std::memory_order_acquire));
    REQUIRE(first_result.success());
    REQUIRE_FALSE(first_result.was_cancelled());
    REQUIRE(first_result.value.has_value());
    REQUIRE(*first_result.value == 7);
    REQUIRE_FALSE(second_result.has_value());
    REQUIRE(ch.empty());
}

TEST_CASE("channel receive restores cancellation after a stale notification",
          "[sync][channel][cancellation][regression]") {
    channel<int> ch(1);
    cancel_source source;
    elio::sync::detail::bounded_recv_waits_for_test.store(
        0, std::memory_order_release);

    std::atomic<bool> blocker_running{false};
    std::atomic<bool> release_blocker{false};
    scheduler sched(2);
    sched.start();

    auto receiver = sched.go_joinable_to(0, [&]()
            -> task<channel<int>::cancellable_recv_result> {
        co_return co_await ch.recv(source.get_token());
    });
    const bool receiver_waited = wait_for_at_least(
        elio::sync::detail::bounded_recv_waits_for_test, 1);

    auto blocker = sched.go_joinable_to(0, [&]() -> task<void> {
        blocker_running.store(true, std::memory_order_release);
        blocker_running.notify_all();
        while (!release_blocker.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        co_return;
    });
    const bool worker_blocked = wait_for_true(blocker_running);

    const bool sent = ch.try_send(7);
    auto stolen = ch.try_recv();
    source.cancel();
    release_blocker.store(true, std::memory_order_release);
    release_blocker.notify_all();

    const bool receiver_completed = wait_for_condition(
        [&] { return receiver.is_ready(); });
    const bool blocker_completed = wait_for_condition(
        [&] { return blocker.is_ready(); });
    if (!receiver_completed) {
        ch.close();
    }
    REQUIRE(wait_for_condition([&] { return receiver.is_ready(); }));
    REQUIRE((blocker_completed ||
             wait_for_condition([&] { return blocker.is_ready(); })));
    receiver.wait_destroyed();
    blocker.wait_destroyed();
    auto result = receiver.await_resume();
    blocker.await_resume();
    REQUIRE(sched.shutdown(std::chrono::milliseconds(2000)));

    REQUIRE(receiver_completed);
    REQUIRE(receiver_waited);
    REQUIRE(worker_blocked);
    REQUIRE(sent);
    REQUIRE(stolen.has_value());
    REQUIRE(*stolen == 7);
    REQUIRE_FALSE(result.success());
    REQUIRE(result.was_cancelled());
    REQUIRE_FALSE(result.was_closed());
}
