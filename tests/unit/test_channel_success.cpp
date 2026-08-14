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
#include <array>
#include <barrier>
#include <cstddef>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>

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

struct send_move_observation {
    bool armed = false;
    size_t frame_transfers = 0;
};

template<size_t InlineBytes = 128>
struct inline_move_only_value {
    std::array<std::byte, InlineBytes> storage{};
    int id = 0;
    send_move_observation* observation = nullptr;
    bool frame_origin = true;

    inline_move_only_value(int value, send_move_observation* observed) noexcept
        : id(value), observation(observed) {}

    inline_move_only_value(const inline_move_only_value&) = delete;
    inline_move_only_value& operator=(const inline_move_only_value&) = delete;

    inline_move_only_value(inline_move_only_value&& other) noexcept
        : storage(other.storage)
        , id(other.id)
        , observation(other.observation)
        , frame_origin(other.frame_origin) {
        finish_move(other);
    }

    inline_move_only_value& operator=(inline_move_only_value&& other) noexcept {
        storage = other.storage;
        id = other.id;
        observation = other.observation;
        frame_origin = other.frame_origin;
        finish_move(other);
        return *this;
    }

private:
    void finish_move(inline_move_only_value& other) noexcept {
        if (frame_origin && observation && observation->armed) {
            ++observation->frame_transfers;
            frame_origin = false;
        }
        other.frame_origin = false;
        other.id = -1;
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

struct bounded_refill_hook_scope {
    bounded_refill_hook_scope() { reset(); }

    ~bounded_refill_hook_scope() {
        elio::sync::detail::pause_bounded_refill_empty_return_for_test.store(
            false, std::memory_order_release);
        elio::sync::detail::pause_bounded_refill_empty_return_for_test.notify_all();
        elio::sync::detail::pause_bounded_refill_after_dequeue_for_test.store(
            false, std::memory_order_release);
        elio::sync::detail::pause_bounded_refill_after_dequeue_for_test.notify_all();
    }

    static void reset() {
        elio::sync::detail::bounded_refill_snapshot_locks_for_test.store(
            0, std::memory_order_release);
        elio::sync::detail::bounded_refill_credit_locks_for_test.store(
            0, std::memory_order_release);
        elio::sync::detail::bounded_refill_empty_returns_for_test.store(
            0, std::memory_order_release);
        elio::sync::detail::bounded_refill_token_skips_for_test.store(
            0, std::memory_order_release);
        elio::sync::detail::bounded_try_send_lock_attempts_for_test.store(
            0, std::memory_order_release);
        elio::sync::detail::pause_bounded_refill_empty_return_for_test.store(
            false, std::memory_order_release);
        elio::sync::detail::bounded_refill_paused_on_empty_for_test.store(
            false, std::memory_order_release);
        elio::sync::detail::pause_bounded_refill_after_dequeue_for_test.store(
            false, std::memory_order_release);
        elio::sync::detail::bounded_refill_paused_after_dequeue_for_test.store(
            false, std::memory_order_release);
    }
};

size_t refill_snapshot_locks() noexcept {
    return elio::sync::detail::bounded_refill_snapshot_locks_for_test.load(
        std::memory_order_relaxed);
}

size_t refill_credit_locks() noexcept {
    return elio::sync::detail::bounded_refill_credit_locks_for_test.load(
        std::memory_order_relaxed);
}

size_t refill_empty_returns() noexcept {
    return elio::sync::detail::bounded_refill_empty_returns_for_test.load(
        std::memory_order_relaxed);
}

size_t refill_token_skips() noexcept {
    return elio::sync::detail::bounded_refill_token_skips_for_test.load(
        std::memory_order_relaxed);
}

}  // namespace

TEST_CASE("channel send frames transfer move-only payloads once",
          "[sync][channel][coro][frame_allocation]") {
    using payload = inline_move_only_value<>;

    SECTION("ready bounded send") {
        channel<payload> ch(1);
        send_move_observation observed;
        auto send_task = ch.send(payload(11, &observed));
        observed.armed = true;

        auto handle = elio::coro::detail::task_access::handle(send_task);
        handle.resume();

        REQUIRE(handle.done());
        REQUIRE(handle.promise().value_.value());
        REQUIRE(observed.frame_transfers == 1);
        auto received = ch.try_recv();
        REQUIRE(received.has_value());
        REQUIRE(received->id == 11);
        REQUIRE(observed.frame_transfers == 1);
    }

    SECTION("ready unbounded send") {
        auto ch = channel<payload>::unbounded();
        send_move_observation observed;
        auto send_task = ch.send(payload(17, &observed));
        observed.armed = true;

        auto handle = elio::coro::detail::task_access::handle(send_task);
        handle.resume();

        REQUIRE(handle.done());
        REQUIRE(handle.promise().value_.value());
        REQUIRE(observed.frame_transfers == 1);
        auto received = ch.try_recv();
        REQUIRE(received.has_value());
        REQUIRE(received->id == 17);
        REQUIRE(observed.frame_transfers == 1);
    }

    SECTION("bounded parked sender refill") {
        channel<payload> ch(1);
        send_move_observation filler_observed;
        send_move_observation observed;
        REQUIRE(ch.try_send(payload(1, &filler_observed)));

        auto send_task = ch.send(payload(12, &observed));
        observed.armed = true;
        auto handle = elio::coro::detail::task_access::handle(send_task);
        handle.resume();
        REQUIRE_FALSE(handle.done());
        REQUIRE(observed.frame_transfers == 0);

        auto first = ch.try_recv();
        REQUIRE(first.has_value());
        REQUIRE(first->id == 1);
        REQUIRE(handle.done());
        REQUIRE(observed.frame_transfers == 1);
        auto second = ch.try_recv();
        REQUIRE(second.has_value());
        REQUIRE(second->id == 12);
        REQUIRE(observed.frame_transfers == 1);
    }

    SECTION("rendezvous receiver steals parked sender") {
        channel<payload> ch;
        send_move_observation observed;
        auto send_task = ch.send(payload(13, &observed));
        observed.armed = true;
        auto handle = elio::coro::detail::task_access::handle(send_task);
        handle.resume();
        REQUIRE_FALSE(handle.done());
        REQUIRE(observed.frame_transfers == 0);

        auto received = ch.try_recv();
        REQUIRE(received.has_value());
        REQUIRE(received->id == 13);
        REQUIRE(handle.done());
        REQUIRE(observed.frame_transfers == 1);
    }

    SECTION("close drains parked sender") {
        channel<payload> ch;
        send_move_observation observed;
        auto send_task = ch.send(payload(14, &observed));
        observed.armed = true;
        auto handle = elio::coro::detail::task_access::handle(send_task);
        handle.resume();
        REQUIRE_FALSE(handle.done());

        ch.close();
        REQUIRE(handle.done());
        REQUIRE(handle.promise().value_.value());
        REQUIRE(observed.frame_transfers == 1);
        auto received = ch.try_recv();
        REQUIRE(received.has_value());
        REQUIRE(received->id == 14);
        REQUIRE(observed.frame_transfers == 1);
    }

    SECTION("cancellation winner leaves frame payload untouched") {
        channel<payload> ch;
        cancel_source source;
        send_move_observation observed;
        auto send_task = ch.send(
            payload(15, &observed), source.get_token());
        observed.armed = true;
        auto handle = elio::coro::detail::task_access::handle(send_task);
        handle.resume();
        REQUIRE_FALSE(handle.done());
        REQUIRE(observed.frame_transfers == 0);

        source.cancel();
        REQUIRE(handle.done());
        REQUIRE(handle.promise().value_->was_cancelled());
        REQUIRE(observed.frame_transfers == 0);
        REQUIRE_FALSE(ch.try_recv().has_value());
    }

    SECTION("active-token ready send transfers once") {
        channel<payload> ch(1);
        cancel_source source;
        send_move_observation observed;
        auto send_task = ch.send(
            payload(16, &observed), source.get_token());
        observed.armed = true;
        auto handle = elio::coro::detail::task_access::handle(send_task);
        handle.resume();

        REQUIRE(handle.done());
        REQUIRE(handle.promise().value_->success());
        REQUIRE(observed.frame_transfers == 1);
        auto received = ch.try_recv();
        REQUIRE(received.has_value());
        REQUIRE(received->id == 16);
        REQUIRE(observed.frame_transfers == 1);
    }

    SECTION("active-token parked send transfers once after refill") {
        channel<payload> ch(1);
        send_move_observation filler_observed;
        send_move_observation observed;
        REQUIRE(ch.try_send(payload(1, &filler_observed)));
        cancel_source source;
        auto send_task = ch.send(
            payload(18, &observed), source.get_token());
        observed.armed = true;
        auto handle = elio::coro::detail::task_access::handle(send_task);
        handle.resume();
        REQUIRE_FALSE(handle.done());
        REQUIRE(observed.frame_transfers == 0);

        auto first = ch.try_recv();
        REQUIRE(first.has_value());
        REQUIRE(first->id == 1);
        REQUIRE(handle.done());
        REQUIRE(handle.promise().value_->success());
        REQUIRE(observed.frame_transfers == 1);
        auto second = ch.try_recv();
        REQUIRE(second.has_value());
        REQUIRE(second->id == 18);
        REQUIRE(observed.frame_transfers == 1);
    }
}

TEST_CASE("channel send races move frame payload only for delivery winners",
          "[sync][channel][cancellation][race][frame_allocation]") {
    using payload = inline_move_only_value<64>;

    SECTION("cancellation versus bounded refill") {
        for (int iteration = 0; iteration < 64; ++iteration) {
            channel<payload> ch(1);
            send_move_observation filler_observed;
            send_move_observation observed;
            REQUIRE(ch.try_send(payload(1, &filler_observed)));
            cancel_source source;
            auto send_task = ch.send(
                payload(30 + iteration, &observed), source.get_token());
            observed.armed = true;
            auto handle = elio::coro::detail::task_access::handle(send_task);
            handle.resume();
            REQUIRE_FALSE(handle.done());

            std::optional<payload> first;
            std::barrier start(3);
            std::thread canceller([&] {
                start.arrive_and_wait();
                source.cancel();
            });
            std::thread receiver([&] {
                start.arrive_and_wait();
                first = ch.try_recv();
            });
            start.arrive_and_wait();
            canceller.join();
            receiver.join();

            REQUIRE(first.has_value());
            REQUIRE(first->id == 1);
            REQUIRE(handle.done());
            const auto& result = handle.promise().value_.value();
            if (result.success()) {
                REQUIRE(observed.frame_transfers == 1);
                auto delivered = ch.try_recv();
                REQUIRE(delivered.has_value());
                REQUIRE(delivered->id == 30 + iteration);
            } else {
                REQUIRE(result.was_cancelled());
                REQUIRE(observed.frame_transfers == 0);
                REQUIRE_FALSE(ch.try_recv().has_value());
            }
        }
    }

    SECTION("cancellation versus close drain") {
        for (int iteration = 0; iteration < 64; ++iteration) {
            channel<payload> ch;
            send_move_observation observed;
            cancel_source source;
            auto send_task = ch.send(
                payload(100 + iteration, &observed), source.get_token());
            observed.armed = true;
            auto handle = elio::coro::detail::task_access::handle(send_task);
            handle.resume();
            REQUIRE_FALSE(handle.done());

            std::barrier start(3);
            std::thread canceller([&] {
                start.arrive_and_wait();
                source.cancel();
            });
            std::thread closer([&] {
                start.arrive_and_wait();
                ch.close();
            });
            start.arrive_and_wait();
            canceller.join();
            closer.join();

            REQUIRE(handle.done());
            const auto& result = handle.promise().value_.value();
            auto delivered = ch.try_recv();
            if (result.success()) {
                REQUIRE(observed.frame_transfers == 1);
                REQUIRE(delivered.has_value());
                REQUIRE(delivered->id == 100 + iteration);
            } else {
                REQUIRE(result.was_cancelled());
                REQUIRE(observed.frame_transfers == 0);
                REQUIRE_FALSE(delivered.has_value());
            }
        }
    }
}

TEST_CASE("public channel send awaiters retain owned move-only values",
          "[sync][channel][cancellation][lifetime]") {
    using payload = inline_move_only_value<64>;
    using owning_awaiter = channel<payload>::send_awaitable;
    STATIC_REQUIRE((std::is_base_of_v<
        elio::detail::intrusive_list_node<owning_awaiter>, owning_awaiter>));

    SECTION("ordinary awaiter") {
        channel<payload> ch;
        send_move_observation observed;
        std::optional<channel<payload>::send_awaitable> sender;
        {
            payload source(21, &observed);
            sender.emplace(ch, std::move(source));
        }

        REQUIRE_FALSE(sender->await_ready());
        REQUIRE(sender->await_suspend(std::noop_coroutine()));
        REQUIRE(sender->is_linked());
        auto received = ch.try_recv();
        REQUIRE(received.has_value());
        REQUIRE(received->id == 21);
        REQUIRE_FALSE(sender->is_linked());
        REQUIRE(sender->await_resume());
    }

    SECTION("cancellable awaiter remains an owning send awaiter") {
        channel<payload> ch;
        send_move_observation observed;
        std::optional<channel<payload>::cancellable_send_awaitable> sender;
        {
            payload source(22, &observed);
            sender.emplace(ch, std::move(source), cancel_token{});
        }

        channel<payload>::send_awaitable& base = *sender;
        REQUIRE_FALSE(base.await_ready());
        REQUIRE(sender->await_suspend(std::noop_coroutine()));
        REQUIRE(base.is_linked());
        auto received = ch.try_recv();
        REQUIRE(received.has_value());
        REQUIRE(received->id == 22);
        REQUIRE_FALSE(base.is_linked());
        REQUIRE(sender->await_resume().success());
    }
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

TEST_CASE("bounded channel skips refill work only for an empty sender queue",
          "[sync][channel][refill][regression]") {
    bounded_refill_hook_scope hooks;

    SECTION("a sender queued before the decision uses the existing refill path") {
        channel<int> ch(1);
        REQUIRE(ch.try_send(1));
        channel<int>::send_awaitable sender(ch, 2);
        REQUIRE(sender.await_suspend(std::noop_coroutine()));

        const auto first = ch.try_recv();

        REQUIRE(first == 1);
        REQUIRE(sender.await_resume());
        REQUIRE(refill_snapshot_locks() == 1);
        REQUIRE(refill_credit_locks() == 1);
        REQUIRE(refill_empty_returns() == 0);
        REQUIRE(ch.try_recv() == 2);
    }

    SECTION("a sender arriving during the empty decision sends after unlock") {
        channel<int> ch(1);
        REQUIRE(ch.try_send(1));
        elio::sync::detail::pause_bounded_refill_empty_return_for_test.store(
            true, std::memory_order_release);

        std::optional<int> first;
        std::thread consumer([&] { first = ch.try_recv(); });
        const bool empty_decision_paused = wait_for_true(
            elio::sync::detail::bounded_refill_paused_on_empty_for_test);
        if (!empty_decision_paused) {
            elio::sync::detail::pause_bounded_refill_empty_return_for_test.store(
                false, std::memory_order_release);
            elio::sync::detail::pause_bounded_refill_empty_return_for_test
                .notify_all();
            consumer.join();
            REQUIRE(empty_decision_paused);
            return;
        }

        std::atomic<bool> sender_done{false};
        bool sent = false;
        elio::sync::detail::bounded_try_send_lock_attempts_for_test.store(
            0, std::memory_order_release);
        std::thread sender([&] {
            sent = ch.try_send(2);
            sender_done.store(true, std::memory_order_release);
            sender_done.notify_all();
        });
        const bool sender_reached_decision = wait_for_at_least(
            elio::sync::detail::bounded_try_send_lock_attempts_for_test, 1);
        const bool sender_blocked_by_decision =
            !sender_done.load(std::memory_order_acquire);

        elio::sync::detail::pause_bounded_refill_empty_return_for_test.store(
            false, std::memory_order_release);
        elio::sync::detail::pause_bounded_refill_empty_return_for_test
            .notify_all();
        sender.join();
        consumer.join();

        REQUIRE(sender_reached_decision);
        REQUIRE(sender_blocked_by_decision);
        REQUIRE(first == 1);
        REQUIRE(sent);
        REQUIRE(refill_snapshot_locks() == 1);
        REQUIRE(refill_credit_locks() == 0);
        REQUIRE(refill_empty_returns() == 1);
        REQUIRE(ch.try_recv() == 2);
    }

    SECTION("a sender arriving after the empty return uses the freed slot") {
        channel<int> ch(1);
        REQUIRE(ch.try_send(1));

        REQUIRE(ch.try_recv() == 1);
        REQUIRE(refill_snapshot_locks() == 1);
        REQUIRE(refill_credit_locks() == 0);
        REQUIRE(refill_empty_returns() == 1);

        REQUIRE(ch.try_send(2));
        REQUIRE(ch.try_recv() == 2);
    }

    SECTION("ordinary receive takes one empty-queue snapshot lock") {
        channel<int> ch(1);
        REQUIRE(ch.try_send(5));
        scheduler sched(1);
        sched.start();
        auto receiver = sched.go_joinable([&]() -> task<std::optional<int>> {
            co_return co_await ch.recv();
        });
        const bool completed = wait_for_condition(
            [&] { return receiver.is_ready(); });
        if (!completed) {
            ch.close();
        }
        REQUIRE(wait_for_condition([&] { return receiver.is_ready(); }));
        receiver.wait_destroyed();
        const auto result = receiver.await_resume();
        REQUIRE(sched.shutdown(std::chrono::milliseconds(2000)));

        REQUIRE(completed);
        REQUIRE(result == 5);
        REQUIRE(refill_snapshot_locks() == 1);
        REQUIRE(refill_credit_locks() == 0);
        REQUIRE(refill_empty_returns() == 1);
    }

    SECTION("token receive reuses its existing channel lock") {
        channel<int> ch(1);
        REQUIRE(ch.try_send(7));
        cancel_source source;
        scheduler sched(1);
        sched.start();
        auto receiver = sched.go_joinable([&]()
                -> task<channel<int>::cancellable_recv_result> {
            co_return co_await ch.recv(source.get_token());
        });
        const bool completed = wait_for_condition(
            [&] { return receiver.is_ready(); });
        if (!completed) {
            ch.close();
        }
        REQUIRE(wait_for_condition([&] { return receiver.is_ready(); }));
        receiver.wait_destroyed();
        const auto result = receiver.await_resume();
        REQUIRE(sched.shutdown(std::chrono::milliseconds(2000)));

        REQUIRE(completed);
        REQUIRE(result.success());
        REQUIRE(result.value == 7);
        REQUIRE(refill_token_skips() == 1);
        REQUIRE(refill_snapshot_locks() == 0);
        REQUIRE(refill_credit_locks() == 0);
        REQUIRE(refill_empty_returns() == 0);
    }
}

TEST_CASE("bounded channel empty refill decision preserves publication handoff",
          "[sync][channel][refill][regression]") {
    bounded_refill_hook_scope hooks;
    auto first_gate = std::make_shared<slot_release_gate>();
    auto second_gate = std::make_shared<slot_release_gate>();
    channel<gated_value> ch(2);
    REQUIRE(ch.try_send(gated_value(1, first_gate)));
    REQUIRE(ch.try_send(gated_value(2, second_gate)));

    first_gate->block_next_move.store(true, std::memory_order_release);
    second_gate->block_next_move.store(true, std::memory_order_release);
    std::optional<gated_value> first;
    std::optional<gated_value> second;
    std::atomic<bool> second_done{false};
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
        second_done.store(true, std::memory_order_release);
        second_done.notify_all();
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

    second_gate->release_move.store(true, std::memory_order_release);
    second_gate->release_move.notify_all();
    const bool second_returned_first = wait_for_true(second_done);

    elio::sync::detail::bounded_send_publish_waits_for_test.store(
        0, std::memory_order_release);
    scheduler sched(1);
    sched.start();
    auto sender = sched.go_joinable([&]() -> task<bool> {
        co_return co_await ch.send(gated_value(3));
    });
    const bool sender_waited_for_next_publication = wait_for_at_least(
        elio::sync::detail::bounded_send_publish_waits_for_test, 1);

    first_gate->release_move.store(true, std::memory_order_release);
    first_gate->release_move.notify_all();
    first_consumer.join();
    second_consumer.join();
    const bool sender_completed_without_cleanup = wait_for_condition(
        [&] { return sender.is_ready(); });
    if (!sender_completed_without_cleanup) {
        ch.close();
    }
    REQUIRE(wait_for_condition([&] { return sender.is_ready(); }));
    sender.wait_destroyed();
    const bool sent = sender.await_resume();
    REQUIRE(sched.shutdown(std::chrono::milliseconds(2000)));

    REQUIRE(first_claimed);
    REQUIRE(second_claimed);
    REQUIRE(second_returned_first);
    REQUIRE(sender_waited_for_next_publication);
    REQUIRE(sender_completed_without_cleanup);
    REQUIRE(first.has_value());
    REQUIRE(first->value == 1);
    REQUIRE(second.has_value());
    REQUIRE(second->value == 2);
    REQUIRE(sent);
    REQUIRE(refill_empty_returns() >= 1);
    REQUIRE(refill_credit_locks() >= 1);
    auto replacement = ch.try_recv();
    REQUIRE(replacement.has_value());
    REQUIRE(replacement->value == 3);
}

TEST_CASE("bounded channel refill skips cancelled senders without stranding FIFO",
          "[sync][channel][refill][cancellation][regression]") {
    bounded_refill_hook_scope hooks;

    SECTION("cancelled head") {
        channel<int> ch(1);
        REQUIRE(ch.try_send(1));
        cancel_source source;
        channel<int>::cancellable_send_awaitable cancelled(
            ch, 2, source.get_token());
        channel<int>::send_awaitable live(ch, 3);
        REQUIRE(cancelled.await_suspend(std::noop_coroutine()));
        REQUIRE(live.await_suspend(std::noop_coroutine()));
        source.cancel();

        const auto first = ch.try_recv();
        const auto cancelled_result = cancelled.await_resume();
        const bool live_result = live.await_resume();

        REQUIRE(first == 1);
        REQUIRE(cancelled_result.was_cancelled());
        REQUIRE(live_result);
        REQUIRE(refill_snapshot_locks() == 1);
        REQUIRE(refill_credit_locks() == 1);
        REQUIRE(refill_empty_returns() == 0);
        REQUIRE(ch.try_recv() == 3);
        REQUIRE_FALSE(ch.try_recv().has_value());
    }

    SECTION("cancelled middle") {
        channel<int> ch(2);
        REQUIRE(ch.try_send(1));
        REQUIRE(ch.try_send(2));
        cancel_source source;
        channel<int>::send_awaitable first_live(ch, 3);
        channel<int>::cancellable_send_awaitable cancelled(
            ch, 4, source.get_token());
        channel<int>::send_awaitable second_live(ch, 5);
        REQUIRE(first_live.await_suspend(std::noop_coroutine()));
        REQUIRE(cancelled.await_suspend(std::noop_coroutine()));
        REQUIRE(second_live.await_suspend(std::noop_coroutine()));
        source.cancel();

        const auto first = ch.try_recv();
        const auto second = ch.try_recv();
        const bool first_live_result = first_live.await_resume();
        const auto cancelled_result = cancelled.await_resume();
        const bool second_live_result = second_live.await_resume();

        REQUIRE(first == 1);
        REQUIRE(second == 2);
        REQUIRE(first_live_result);
        REQUIRE(cancelled_result.was_cancelled());
        REQUIRE(second_live_result);
        REQUIRE(refill_snapshot_locks() == 2);
        REQUIRE(refill_credit_locks() == 2);
        REQUIRE(refill_empty_returns() == 0);
        REQUIRE(ch.try_recv() == 3);
        REQUIRE(ch.try_recv() == 5);
        REQUIRE_FALSE(ch.try_recv().has_value());
    }
}

TEST_CASE("bounded channel refill retains a dequeued sender wake across close",
          "[sync][channel][refill][lifetime][regression]") {
    bounded_refill_hook_scope hooks;
    channel<int> ch(1);
    REQUIRE(ch.try_send(1));

    auto sender_task = ch.send(2);
    auto sender = elio::coro::detail::task_access::release(
        std::move(sender_task));
    sender.resume();
    REQUIRE_FALSE(sender.done());

    elio::sync::detail::pause_bounded_refill_after_dequeue_for_test.store(
        true, std::memory_order_release);
    std::optional<int> first;
    std::thread consumer([&] { first = ch.try_recv(); });
    const bool refill_dequeued_sender = wait_for_true(
        elio::sync::detail::bounded_refill_paused_after_dequeue_for_test);
    if (!refill_dequeued_sender) {
        elio::sync::detail::pause_bounded_refill_after_dequeue_for_test.store(
            false, std::memory_order_release);
        elio::sync::detail::pause_bounded_refill_after_dequeue_for_test
            .notify_all();
        consumer.join();
        sender.destroy();
        ch.close();
        REQUIRE(refill_dequeued_sender);
        return;
    }

    sender.destroy();
    ch.close();
    elio::sync::detail::pause_bounded_refill_after_dequeue_for_test.store(
        false, std::memory_order_release);
    elio::sync::detail::pause_bounded_refill_after_dequeue_for_test.notify_all();
    consumer.join();

    REQUIRE(first == 1);
    REQUIRE(refill_credit_locks() == 1);
    REQUIRE(ch.try_recv() == 2);
    REQUIRE_FALSE(ch.try_recv().has_value());
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

TEST_CASE("channel receive restores cancellation after a failed fast pop",
          "[sync][channel][cancellation][regression]") {
    auto gate = std::make_shared<slot_release_gate>();
    channel<gated_value> ch(1);
    REQUIRE(ch.try_send(gated_value(7, gate)));
    gate->block_next_move.store(true, std::memory_order_release);

    cancel_source source;
    elio::sync::detail::bounded_recv_paused_after_claim_for_test.store(
        false, std::memory_order_release);
    elio::sync::detail::pause_bounded_recv_after_claim_for_test.store(
        true, std::memory_order_release);
    elio::sync::detail::bounded_recv_paused_after_failed_pop_for_test.store(
        false, std::memory_order_release);
    elio::sync::detail::pause_bounded_recv_after_failed_pop_for_test.store(
        true, std::memory_order_release);

    scheduler sched(1);
    sched.start();
    auto receiver = sched.go_joinable([&]()
            -> task<channel<gated_value>::cancellable_recv_result> {
        co_return co_await ch.recv(source.get_token());
    });
    const bool receiver_claimed_completion = wait_for_true(
        elio::sync::detail::bounded_recv_paused_after_claim_for_test);

    std::optional<gated_value> stolen;
    std::thread stealing_consumer([&] { stolen = ch.try_recv(); });
    const bool stealing_consumer_claimed_slot = wait_for_true(
        gate->move_blocked);

    source.cancel();
    elio::sync::detail::pause_bounded_recv_after_claim_for_test.store(
        false, std::memory_order_release);
    elio::sync::detail::pause_bounded_recv_after_claim_for_test.notify_all();

    const bool receiver_observed_failed_pop = wait_for_true(
        elio::sync::detail::bounded_recv_paused_after_failed_pop_for_test);
    gate->release_move.store(true, std::memory_order_release);
    gate->release_move.notify_all();
    stealing_consumer.join();
    const bool replacement_sent = ch.try_send(gated_value(8));

    elio::sync::detail::pause_bounded_recv_after_failed_pop_for_test.store(
        false, std::memory_order_release);
    elio::sync::detail::pause_bounded_recv_after_failed_pop_for_test.notify_all();

    const bool receiver_completed_without_cleanup = wait_for_condition(
        [&] { return receiver.is_ready(); });
    if (!receiver_completed_without_cleanup) {
        ch.close();
    }
    REQUIRE(wait_for_condition([&] { return receiver.is_ready(); }));
    receiver.wait_destroyed();
    auto result = receiver.await_resume();
    REQUIRE(sched.shutdown(std::chrono::milliseconds(2000)));

    REQUIRE(receiver_completed_without_cleanup);
    REQUIRE(receiver_claimed_completion);
    REQUIRE(stealing_consumer_claimed_slot);
    REQUIRE(receiver_observed_failed_pop);
    REQUIRE(stolen.has_value());
    REQUIRE(stolen->value == 7);
    REQUIRE(replacement_sent);
    REQUIRE_FALSE(result.success());
    REQUIRE(result.was_cancelled());
    REQUIRE_FALSE(result.was_closed());
    auto replacement = ch.try_recv();
    REQUIRE(replacement.has_value());
    REQUIRE(replacement->value == 8);
}
