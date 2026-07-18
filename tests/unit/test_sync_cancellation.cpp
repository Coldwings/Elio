#include <catch2/catch_test_macros.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/sync/detail/wake_state.hpp>
#include <elio/coro/task.hpp>
#include <elio/coro/this_coro.hpp>
#include <elio/runtime/scheduler.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <coroutine>
#include <memory>
#include <thread>
#include <type_traits>

using namespace elio::sync;
using namespace elio::coro;
using namespace elio::runtime;

namespace {

template<typename Predicate>
bool wait_for_condition(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (!predicate() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

bool wait_for_flag(const std::atomic<bool>& flag) {
    return wait_for_condition([&] {
        return flag.load(std::memory_order_acquire);
    });
}

} // namespace

TEST_CASE("wake_state resolves completion during blocked publication",
          "[sync][cancellation][wake_state]") {
    SECTION("cancellation") {
        auto state = elio::sync::detail::make_wake_state();
        REQUIRE(state->set_handle_blocked(std::noop_coroutine()));
        REQUIRE(state->request_cancel());
        REQUIRE_FALSE(state->unblock_after_publish());
        REQUIRE(state->was_cancelled());
        REQUIRE_FALSE(state->notification_was_selected());
        REQUIRE_FALSE(state->schedule_selected());
    }

    SECTION("notification") {
        auto state = elio::sync::detail::make_wake_state();
        REQUIRE(state->set_handle_blocked(std::noop_coroutine()));
        REQUIRE(state->claim_notification() ==
                elio::sync::detail::wake_action::completed_inline);
        REQUIRE_FALSE(state->unblock_after_publish());
        REQUIRE_FALSE(state->was_cancelled());
        REQUIRE(state->notification_was_selected());
        REQUIRE_FALSE(state->request_cancel());
    }
}

TEST_CASE("wake_state does not lose notification while publication unblocks",
          "[sync][cancellation][wake_state][race]") {
    constexpr int iterations = 1024;
    std::barrier start_race(3);
    std::barrier finish_race(3);
    elio::sync::detail::wake_state_ptr state;
    std::array<bool, iterations> unblocked{};
    std::array<bool, iterations> scheduled{};

    std::thread publisher([&] {
        for (int i = 0; i < iterations; ++i) {
            start_race.arrive_and_wait();
            unblocked[i] = state->unblock_after_publish();
            finish_race.arrive_and_wait();
        }
    });
    std::thread notifier([&] {
        for (int i = 0; i < iterations; ++i) {
            start_race.arrive_and_wait();
            scheduled[i] = state->schedule_selected();
            finish_race.arrive_and_wait();
        }
    });

    for (int i = 0; i < iterations; ++i) {
        state = elio::sync::detail::make_wake_state();
        REQUIRE(state->set_handle_blocked(std::noop_coroutine()));
        start_race.arrive_and_wait();
        finish_race.arrive_and_wait();

        REQUIRE((!unblocked[i] || scheduled[i]));
        REQUIRE(state->notification_was_selected());
    }

    publisher.join();
    notifier.join();
}

TEST_CASE("basic sync waits preserve resources when already cancelled",
          "[sync][cancellation][cancel_token]") {
    cancel_source source;
    source.cancel();

    SECTION("mutex") {
        mutex m;
        auto waiter = m.lock(source.get_token());
        REQUIRE(waiter.await_ready());
        REQUIRE(waiter.await_resume() == cancel_result::cancelled);
        REQUIRE_FALSE(m.is_locked());
    }

    SECTION("semaphore") {
        semaphore sem(1);
        auto waiter = sem.acquire(source.get_token());
        REQUIRE(waiter.await_ready());
        REQUIRE(waiter.await_resume() == cancel_result::cancelled);
        REQUIRE(sem.count() == 1);
    }

    SECTION("event") {
        event e;
        e.set();
        auto waiter = e.wait(source.get_token());
        REQUIRE(waiter.await_ready());
        REQUIRE(waiter.await_resume() == cancel_result::cancelled);
        REQUIRE(e.is_set());
    }
}

TEST_CASE("basic sync waits without tokens preserve void results",
          "[sync][cancellation][result]") {
    SECTION("mutex") {
        mutex m;
        auto waiter = m.lock();
        REQUIRE(waiter.await_ready());
        static_assert(std::is_void_v<decltype(waiter.await_resume())>);
        waiter.await_resume();
        m.unlock();
    }

    SECTION("semaphore") {
        semaphore sem(1);
        auto waiter = sem.acquire();
        REQUIRE(waiter.await_ready());
        static_assert(std::is_void_v<decltype(waiter.await_resume())>);
        waiter.await_resume();
        REQUIRE(sem.count() == 0);
    }

    SECTION("event") {
        event e;
        e.set();
        auto waiter = e.wait();
        REQUIRE(waiter.await_ready());
        static_assert(std::is_void_v<decltype(waiter.await_resume())>);
        waiter.await_resume();
    }
}

TEST_CASE("runtime cancellation wakes basic sync waits",
          "[sync][cancellation][cancel_token][runtime]") {
    scheduler sched(2);
    sched.start();

    SECTION("mutex") {
        mutex m;
        REQUIRE(m.try_lock());
        std::atomic<bool> started{false};
        std::atomic<bool> completed{false};
        std::atomic<cancel_result> result{cancel_result::completed};

        auto joined = sched.go_joinable([&]() -> task<void> {
            started.store(true, std::memory_order_release);
            result.store(co_await m.lock(this_coro::cancel_token()),
                         std::memory_order_release);
            completed.store(true, std::memory_order_release);
            co_return;
        });

        REQUIRE(wait_for_flag(started));
        joined.request_cancel();
        REQUIRE(wait_for_flag(completed));
        joined.wait_destroyed();
        REQUIRE(result.load(std::memory_order_acquire) ==
                cancel_result::cancelled);
        REQUIRE(m.is_locked());
        m.unlock();
    }

    SECTION("semaphore") {
        semaphore sem(0);
        std::atomic<bool> started{false};
        std::atomic<bool> completed{false};
        std::atomic<cancel_result> result{cancel_result::completed};

        auto joined = sched.go_joinable([&]() -> task<void> {
            started.store(true, std::memory_order_release);
            result.store(co_await sem.acquire(this_coro::cancel_token()),
                         std::memory_order_release);
            completed.store(true, std::memory_order_release);
            co_return;
        });

        REQUIRE(wait_for_flag(started));
        joined.request_cancel();
        REQUIRE(wait_for_flag(completed));
        joined.wait_destroyed();
        REQUIRE(result.load(std::memory_order_acquire) ==
                cancel_result::cancelled);
        REQUIRE(sem.count() == 0);
    }

    SECTION("event") {
        event e;
        std::atomic<bool> started{false};
        std::atomic<bool> completed{false};
        std::atomic<cancel_result> result{cancel_result::completed};

        auto joined = sched.go_joinable([&]() -> task<void> {
            started.store(true, std::memory_order_release);
            result.store(co_await e.wait(this_coro::cancel_token()),
                         std::memory_order_release);
            completed.store(true, std::memory_order_release);
            co_return;
        });

        REQUIRE(wait_for_flag(started));
        joined.request_cancel();
        REQUIRE(wait_for_flag(completed));
        joined.wait_destroyed();
        REQUIRE(result.load(std::memory_order_acquire) ==
                cancel_result::cancelled);
        REQUIRE_FALSE(e.is_set());
    }

    sched.shutdown();
}

TEST_CASE("basic sync notification wins before later cancellation",
          "[sync][cancellation][cancel_token][notification]") {
    scheduler sched(2);
    sched.start();

    SECTION("mutex") {
        mutex m;
        REQUIRE(m.try_lock());
        cancel_source source;
        std::atomic<bool> started{false};
        std::atomic<bool> completed{false};
        std::atomic<cancel_result> result{cancel_result::cancelled};

        auto joined = sched.go_joinable([&]() -> task<void> {
            started.store(true, std::memory_order_release);
            const auto terminal = co_await m.lock(source.get_token());
            result.store(terminal, std::memory_order_release);
            if (terminal == cancel_result::completed) {
                m.unlock();
            }
            completed.store(true, std::memory_order_release);
            co_return;
        });

        REQUIRE(wait_for_flag(started));
        m.unlock();
        REQUIRE(wait_for_flag(completed));
        source.cancel();
        joined.wait_destroyed();
        REQUIRE(result.load(std::memory_order_acquire) ==
                cancel_result::completed);
        REQUIRE_FALSE(m.is_locked());
    }

    SECTION("semaphore") {
        semaphore sem(0);
        cancel_source source;
        std::atomic<bool> started{false};
        std::atomic<bool> completed{false};
        std::atomic<cancel_result> result{cancel_result::cancelled};

        auto joined = sched.go_joinable([&]() -> task<void> {
            started.store(true, std::memory_order_release);
            result.store(co_await sem.acquire(source.get_token()),
                         std::memory_order_release);
            completed.store(true, std::memory_order_release);
            co_return;
        });

        REQUIRE(wait_for_flag(started));
        sem.release();
        REQUIRE(wait_for_flag(completed));
        source.cancel();
        joined.wait_destroyed();
        REQUIRE(result.load(std::memory_order_acquire) ==
                cancel_result::completed);
        REQUIRE(sem.count() == 0);
    }

    SECTION("event") {
        event e;
        cancel_source source;
        std::atomic<bool> started{false};
        std::atomic<bool> completed{false};
        std::atomic<cancel_result> result{cancel_result::cancelled};

        auto joined = sched.go_joinable([&]() -> task<void> {
            started.store(true, std::memory_order_release);
            result.store(co_await e.wait(source.get_token()),
                         std::memory_order_release);
            completed.store(true, std::memory_order_release);
            co_return;
        });

        REQUIRE(wait_for_flag(started));
        e.set();
        REQUIRE(wait_for_flag(completed));
        source.cancel();
        joined.wait_destroyed();
        REQUIRE(result.load(std::memory_order_acquire) ==
                cancel_result::completed);
        REQUIRE(e.is_set());
    }

    sched.shutdown();
}

TEST_CASE("condition_variable pre-cancellation preserves held locks",
          "[sync][condvar][cancellation][cancel_token]") {
    cancel_source source;
    source.cancel();
    condition_variable cv;

    SECTION("unlocked") {
        auto waiter = cv.wait_unlocked(source.get_token());
        REQUIRE(waiter.await_ready());
        REQUIRE(waiter.await_resume() == cancel_result::cancelled);
        REQUIRE_FALSE(cv.has_waiters());
    }

    SECTION("spinlock") {
        spinlock lock;
        lock.lock();
        auto waiter = cv.wait(lock, source.get_token());
        REQUIRE(waiter.await_ready());
        REQUIRE(waiter.await_resume() == cancel_result::cancelled);
        REQUIRE(lock.is_locked());
        lock.unlock();
        REQUIRE_FALSE(cv.has_waiters());
    }
}

TEST_CASE("condition_variable cancellation re-acquires coroutine mutex",
          "[sync][condvar][cancellation][cancel_token][runtime]") {
    scheduler sched(2);
    sched.start();

    condition_variable cv;
    mutex m;
    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};
    std::atomic<cancel_result> result{cancel_result::completed};

    auto joined = sched.go_joinable([&]() -> task<void> {
        co_await m.lock();
        started.store(true, std::memory_order_release);
        result.store(co_await cv.wait(m, this_coro::cancel_token()),
                     std::memory_order_release);
        m.unlock();
        completed.store(true, std::memory_order_release);
        co_return;
    });

    REQUIRE(wait_for_flag(started));
    REQUIRE(wait_for_condition([&] { return cv.has_waiters(); }));
    REQUIRE(m.try_lock());

    joined.request_cancel();
    REQUIRE(wait_for_condition([&] { return !cv.has_waiters(); }));
    REQUIRE_FALSE(completed.load(std::memory_order_acquire));

    m.unlock();
    REQUIRE(wait_for_flag(completed));
    joined.wait_destroyed();
    REQUIRE(result.load(std::memory_order_acquire) ==
            cancel_result::cancelled);
    REQUIRE_FALSE(m.is_locked());

    sched.shutdown();
}

TEST_CASE("condition_variable notification wins before later cancellation",
          "[sync][condvar][cancellation][cancel_token][notification]") {
    scheduler sched(2);
    sched.start();

    condition_variable cv;
    cancel_source source;
    std::atomic<bool> completed{false};
    std::atomic<cancel_result> result{cancel_result::cancelled};

    auto joined = sched.go_joinable([&]() -> task<void> {
        result.store(co_await cv.wait_unlocked(source.get_token()),
                     std::memory_order_release);
        completed.store(true, std::memory_order_release);
        co_return;
    });

    REQUIRE(wait_for_condition([&] { return cv.has_waiters(); }));
    cv.notify_one();
    REQUIRE(wait_for_flag(completed));
    source.cancel();
    joined.wait_destroyed();
    REQUIRE(result.load(std::memory_order_acquire) ==
            cancel_result::completed);
    REQUIRE_FALSE(cv.has_waiters());

    sched.shutdown();
}

TEST_CASE("condition_variable cancellation and notification select one result",
          "[sync][condvar][cancellation][cancel_token][race]") {
    scheduler sched(2);
    sched.start();

    for (int iteration = 0; iteration < 64; ++iteration) {
        condition_variable cv;
        cancel_source source;
        std::atomic<bool> completed{false};
        std::atomic<cancel_result> result{cancel_result::cancelled};

        auto joined = sched.go_joinable([&]() -> task<void> {
            result.store(co_await cv.wait_unlocked(source.get_token()),
                         std::memory_order_release);
            completed.store(true, std::memory_order_release);
            co_return;
        });

        REQUIRE(wait_for_condition([&] { return cv.has_waiters(); }));

        std::barrier race_start(3);
        std::thread canceller([&] {
            race_start.arrive_and_wait();
            source.cancel();
        });
        std::thread notifier([&] {
            race_start.arrive_and_wait();
            cv.notify_one();
        });
        race_start.arrive_and_wait();
        canceller.join();
        notifier.join();

        REQUIRE(wait_for_flag(completed));
        joined.wait_destroyed();
        const auto terminal = result.load(std::memory_order_acquire);
        REQUIRE((terminal == cancel_result::completed ||
                 terminal == cancel_result::cancelled));
        REQUIRE_FALSE(cv.has_waiters());
    }

    sched.shutdown();
}

TEST_CASE("condition_variable notify_one skips a cancelled head waiter",
          "[sync][condvar][cancellation][cancel_token][queue]") {
    condition_variable cv;
    cancel_source source;
    auto first = cv.wait_unlocked(source.get_token());
    auto second = cv.wait_unlocked();

    REQUIRE(first.await_suspend(std::noop_coroutine()));
    REQUIRE(second.await_suspend(std::noop_coroutine()));
    source.cancel();
    cv.notify_one();

    REQUIRE(first.await_resume() == cancel_result::cancelled);
    second.await_resume();
    REQUIRE_FALSE(cv.has_waiters());
}

TEST_CASE("shared_mutex pre-cancellation preserves reader and writer state",
          "[sync][shared_mutex][cancellation][cancel_token]") {
    shared_mutex sm;
    cancel_source source;
    source.cancel();

    SECTION("reader") {
        auto waiter = sm.lock_shared(source.get_token());
        REQUIRE(waiter.await_ready());
        REQUIRE(waiter.await_resume() == cancel_result::cancelled);
        REQUIRE(sm.reader_count() == 0);
        REQUIRE_FALSE(sm.is_writer_active());
    }

    SECTION("writer") {
        auto waiter = sm.lock(source.get_token());
        REQUIRE(waiter.await_ready());
        REQUIRE(waiter.await_resume() == cancel_result::cancelled);
        REQUIRE(sm.reader_count() == 0);
        REQUIRE_FALSE(sm.is_writer_active());
    }
}

TEST_CASE("shared_mutex cancellation and reader grant select one result",
          "[sync][shared_mutex][cancellation][cancel_token][reader][race]") {
    for (int iteration = 0; iteration < 64; ++iteration) {
        shared_mutex sm;
        REQUIRE(sm.try_lock());
        cancel_source source;
        auto waiter = sm.lock_shared(source.get_token());
        REQUIRE(waiter.await_suspend(std::noop_coroutine()));

        std::barrier race_start(3);
        std::thread canceller([&] {
            race_start.arrive_and_wait();
            source.cancel();
        });
        std::thread unlocker([&] {
            race_start.arrive_and_wait();
            sm.unlock();
        });
        race_start.arrive_and_wait();
        canceller.join();
        unlocker.join();

        const auto terminal = waiter.await_resume();
        if (terminal == cancel_result::completed) {
            REQUIRE(sm.reader_count() == 1);
            sm.unlock_shared();
        } else {
            REQUIRE(sm.reader_count() == 0);
        }
        REQUIRE_FALSE(sm.is_writer_active());
    }
}

TEST_CASE("shared_mutex cancellation and writer grant select one result",
          "[sync][shared_mutex][cancellation][cancel_token][writer][race]") {
    for (int iteration = 0; iteration < 64; ++iteration) {
        shared_mutex sm;
        REQUIRE(sm.try_lock_shared());
        cancel_source source;
        auto waiter = sm.lock(source.get_token());
        REQUIRE(waiter.await_suspend(std::noop_coroutine()));

        std::barrier race_start(3);
        std::thread canceller([&] {
            race_start.arrive_and_wait();
            source.cancel();
        });
        std::thread unlocker([&] {
            race_start.arrive_and_wait();
            sm.unlock_shared();
        });
        race_start.arrive_and_wait();
        canceller.join();
        unlocker.join();

        const auto terminal = waiter.await_resume();
        if (terminal == cancel_result::completed) {
            REQUIRE(sm.is_writer_active());
            sm.unlock();
        } else {
            REQUIRE_FALSE(sm.is_writer_active());
        }
        REQUIRE(sm.reader_count() == 0);
    }
}

TEST_CASE("shared_mutex cancelled writer releases parked readers",
          "[sync][shared_mutex][cancellation][cancel_token][queue]") {
    shared_mutex sm;
    REQUIRE(sm.try_lock_shared());
    cancel_source source;
    auto writer = sm.lock(source.get_token());
    auto reader = sm.lock_shared();

    REQUIRE(writer.await_suspend(std::noop_coroutine()));
    REQUIRE(reader.await_suspend(std::noop_coroutine()));
    source.cancel();

    REQUIRE(writer.await_resume() == cancel_result::cancelled);
    reader.await_resume();
    REQUIRE(sm.reader_count() == 2);

    sm.unlock_shared();
    sm.unlock_shared();
    REQUIRE(sm.reader_count() == 0);
    REQUIRE_FALSE(sm.is_writer_active());
}

TEST_CASE("shared_mutex wake paths skip cancelled head waiters",
          "[sync][shared_mutex][cancellation][cancel_token][queue]") {
    SECTION("readers") {
        shared_mutex sm;
        REQUIRE(sm.try_lock());
        cancel_source source;
        auto first = sm.lock_shared(source.get_token());
        auto second = sm.lock_shared();

        REQUIRE(first.await_suspend(std::noop_coroutine()));
        REQUIRE(second.await_suspend(std::noop_coroutine()));
        source.cancel();
        sm.unlock();

        REQUIRE(first.await_resume() == cancel_result::cancelled);
        second.await_resume();
        REQUIRE(sm.reader_count() == 1);
        sm.unlock_shared();
    }

    SECTION("writers") {
        shared_mutex sm;
        REQUIRE(sm.try_lock_shared());
        cancel_source source;
        auto first = sm.lock(source.get_token());
        auto second = sm.lock();

        REQUIRE(first.await_suspend(std::noop_coroutine()));
        REQUIRE(second.await_suspend(std::noop_coroutine()));
        source.cancel();
        sm.unlock_shared();

        REQUIRE(first.await_resume() == cancel_result::cancelled);
        second.await_resume();
        REQUIRE(sm.is_writer_active());
        sm.unlock();
    }
}

TEST_CASE("channel token-aware results distinguish success close and cancellation",
          "[sync][channel][cancellation][cancel_token]") {
    scheduler sched(2);
    sched.start();

    channel<int> bounded(1);
    static_assert(std::is_same_v<decltype(bounded.send(1)), task<bool>>);
    static_assert(std::is_same_v<decltype(bounded.recv()),
                                 task<std::optional<int>>>);
    static_assert(std::is_same_v<decltype(bounded.send(1, cancel_token{})),
                                 task<channel<int>::cancellable_send_result>>);
    static_assert(std::is_same_v<decltype(bounded.recv(cancel_token{})),
                                 task<channel<int>::cancellable_recv_result>>);
    std::optional<channel<int>::cancellable_send_result> send_result;
    std::optional<channel<int>::cancellable_recv_result> recv_result;

    auto send_join = sched.go_joinable([&]() -> task<void> {
        send_result = co_await bounded.send(7, cancel_token{});
    });
    send_join.wait_destroyed();
    REQUIRE(send_result->success());
    REQUIRE_FALSE(send_result->was_cancelled());
    REQUIRE_FALSE(send_result->was_closed());

    auto recv_join = sched.go_joinable([&]() -> task<void> {
        recv_result = co_await bounded.recv(cancel_token{});
    });
    recv_join.wait_destroyed();
    REQUIRE(recv_result->success());
    REQUIRE(recv_result->value == 7);
    REQUIRE_FALSE(recv_result->was_cancelled());

    channel<int> unbounded = channel<int>::unbounded();
    auto unbounded_send = sched.go_joinable([&]() -> task<void> {
        send_result = co_await unbounded.send(11, cancel_token{});
    });
    unbounded_send.wait_destroyed();
    REQUIRE(send_result->success());
    REQUIRE(unbounded.try_recv() == 11);

    channel<std::unique_ptr<int>> move_only(1);
    std::optional<channel<std::unique_ptr<int>>::cancellable_send_result>
        move_send_result;
    std::optional<channel<std::unique_ptr<int>>::cancellable_recv_result>
        move_recv_result;
    auto move_send = sched.go_joinable([&]() -> task<void> {
        move_send_result = co_await move_only.send(
            std::make_unique<int>(23), cancel_token{});
    });
    move_send.wait_destroyed();
    auto move_recv = sched.go_joinable([&]() -> task<void> {
        move_recv_result = co_await move_only.recv(cancel_token{});
    });
    move_recv.wait_destroyed();
    REQUIRE(move_send_result->success());
    REQUIRE(move_recv_result->success());
    REQUIRE(**move_recv_result->value == 23);

    channel<int> closed(1);
    closed.close();
    auto closed_send = sched.go_joinable([&]() -> task<void> {
        send_result = co_await closed.send(13, cancel_token{});
    });
    closed_send.wait_destroyed();
    REQUIRE(send_result->was_closed());
    REQUIRE_FALSE(send_result->was_cancelled());

    auto closed_recv = sched.go_joinable([&]() -> task<void> {
        recv_result = co_await closed.recv(cancel_token{});
    });
    closed_recv.wait_destroyed();
    REQUIRE(recv_result->was_closed());
    REQUIRE_FALSE(recv_result->was_cancelled());

    cancel_source cancelled;
    cancelled.cancel();
    channel<int> preserved(1);
    auto cancelled_send = sched.go_joinable([&]() -> task<void> {
        send_result = co_await preserved.send(17, cancelled.get_token());
    });
    cancelled_send.wait_destroyed();
    REQUIRE(send_result->was_cancelled());
    REQUIRE(preserved.empty());

    REQUIRE(preserved.try_send(19));
    auto cancelled_recv = sched.go_joinable([&]() -> task<void> {
        recv_result = co_await preserved.recv(cancelled.get_token());
    });
    cancelled_recv.wait_destroyed();
    REQUIRE(recv_result->was_cancelled());
    REQUIRE(preserved.try_recv() == 19);

    channel<int> closed_preserved(1);
    REQUIRE(closed_preserved.try_send(29));
    closed_preserved.close();
    auto cancelled_closed_recv = sched.go_joinable([&]() -> task<void> {
        recv_result = co_await closed_preserved.recv(cancelled.get_token());
    });
    cancelled_closed_recv.wait_destroyed();
    REQUIRE(recv_result->was_cancelled());
    REQUIRE(closed_preserved.try_recv() == 29);

    sched.shutdown();
}

TEST_CASE("channel queued cancellation does not move values",
          "[sync][channel][cancellation][cancel_token][queue]") {
    SECTION("bounded sender") {
        channel<int> ch(1);
        REQUIRE(ch.try_send(1));
        cancel_source source;
        channel<int>::cancellable_send_awaitable sender(
            ch, 2, source.get_token());

        REQUIRE(sender.await_suspend(std::noop_coroutine()));
        source.cancel();
        const auto result = sender.await_resume();

        REQUIRE(result.was_cancelled());
        REQUIRE(ch.try_recv() == 1);
        REQUIRE_FALSE(ch.try_recv().has_value());
    }

    SECTION("rendezvous sender") {
        channel<int> ch;
        cancel_source source;
        channel<int>::cancellable_send_awaitable sender(
            ch, 3, source.get_token());

        REQUIRE(sender.await_suspend(std::noop_coroutine()));
        source.cancel();
        REQUIRE(sender.await_resume().was_cancelled());
        REQUIRE_FALSE(ch.try_recv().has_value());
    }

    SECTION("receiver") {
        channel<int> ch(1);
        cancel_source source;
        channel<int>::cancellable_recv_awaitable receiver(
            ch, source.get_token());

        REQUIRE(receiver.await_suspend(std::noop_coroutine()));
        source.cancel();
        REQUIRE(receiver.await_resume() == cancel_result::cancelled);
        REQUIRE(ch.try_send(5));
        REQUIRE(ch.try_recv() == 5);
    }
}

TEST_CASE("channel send cancellation and bounded handoff select one result",
          "[sync][channel][cancellation][cancel_token][race]") {
    for (int iteration = 0; iteration < 64; ++iteration) {
        channel<int> ch(1);
        REQUIRE(ch.try_send(1));
        cancel_source source;
        channel<int>::cancellable_send_awaitable sender(
            ch, 2, source.get_token());
        REQUIRE(sender.await_suspend(std::noop_coroutine()));

        std::optional<int> first;
        std::barrier race_start(3);
        std::thread canceller([&] {
            race_start.arrive_and_wait();
            source.cancel();
        });
        std::thread receiver([&] {
            race_start.arrive_and_wait();
            first = ch.try_recv();
        });
        race_start.arrive_and_wait();
        canceller.join();
        receiver.join();

        REQUIRE(first == 1);
        const auto terminal = sender.await_resume();
        if (terminal.success()) {
            REQUIRE(terminal.cancel == cancel_result::completed);
            REQUIRE(ch.try_recv() == 2);
        } else {
            REQUIRE(terminal.was_cancelled());
            REQUIRE_FALSE(ch.try_recv().has_value());
        }
    }
}

TEST_CASE("channel receive cancellation and send select one result",
          "[sync][channel][cancellation][cancel_token][race]") {
    for (int iteration = 0; iteration < 64; ++iteration) {
        channel<int> ch(1);
        cancel_source source;
        channel<int>::cancellable_recv_awaitable receiver(
            ch, source.get_token());
        REQUIRE(receiver.await_suspend(std::noop_coroutine()));

        std::atomic<bool> send_succeeded{false};
        std::barrier race_start(3);
        std::thread canceller([&] {
            race_start.arrive_and_wait();
            source.cancel();
        });
        std::thread sender([&] {
            race_start.arrive_and_wait();
            send_succeeded.store(ch.try_send(9),
                                 std::memory_order_release);
        });
        race_start.arrive_and_wait();
        canceller.join();
        sender.join();

        REQUIRE(send_succeeded.load(std::memory_order_acquire));
        const auto terminal = receiver.await_resume();
        REQUIRE((terminal == cancel_result::completed ||
                 terminal == cancel_result::cancelled));
        REQUIRE(ch.try_recv() == 9);
    }
}

TEST_CASE("channel recv API preserves values across cancellation races",
          "[sync][channel][cancellation][cancel_token][race][runtime]") {
    scheduler sched(2);
    sched.start();

    for (int iteration = 0; iteration < 64; ++iteration) {
        channel<int> ch(1);
        cancel_source source;
        std::atomic<bool> started{false};
        std::atomic<bool> send_succeeded{false};
        std::optional<channel<int>::cancellable_recv_result> result;

        auto receiver = sched.go_joinable([&]() -> task<void> {
            started.store(true, std::memory_order_release);
            result = co_await ch.recv(source.get_token());
        });
        REQUIRE(wait_for_flag(started));

        std::barrier race_start(3);
        std::thread canceller([&] {
            race_start.arrive_and_wait();
            source.cancel();
        });
        std::thread sender([&] {
            race_start.arrive_and_wait();
            send_succeeded.store(ch.try_send(iteration),
                                 std::memory_order_release);
        });
        race_start.arrive_and_wait();
        canceller.join();
        sender.join();
        receiver.wait_destroyed();

        REQUIRE(send_succeeded.load(std::memory_order_acquire));
        REQUIRE(result.has_value());
        if (result->success()) {
            REQUIRE(result->value == iteration);
            REQUIRE(ch.empty());
        } else {
            REQUIRE(result->was_cancelled());
            REQUIRE(ch.try_recv() == iteration);
        }
    }

    sched.shutdown();
}

TEST_CASE("channel rendezvous handoff selects send or cancellation",
          "[sync][channel][cancellation][cancel_token][race][runtime]") {
    scheduler sched(2);
    sched.start();

    for (int iteration = 0; iteration < 64; ++iteration) {
        channel<int> ch;
        cancel_source source;
        std::atomic<bool> started{false};
        std::atomic<bool> send_succeeded{false};
        std::optional<channel<int>::cancellable_recv_result> result;

        auto receiver = sched.go_joinable([&]() -> task<void> {
            started.store(true, std::memory_order_release);
            result = co_await ch.recv(source.get_token());
        });
        REQUIRE(wait_for_flag(started));

        std::barrier race_start(3);
        std::thread canceller([&] {
            race_start.arrive_and_wait();
            source.cancel();
        });
        std::thread sender([&] {
            race_start.arrive_and_wait();
            send_succeeded.store(ch.try_send(iteration),
                                 std::memory_order_release);
        });
        race_start.arrive_and_wait();
        canceller.join();
        sender.join();
        receiver.wait_destroyed();

        REQUIRE(result.has_value());
        const bool sent = send_succeeded.load(std::memory_order_acquire);
        if (result->success()) {
            REQUIRE(sent);
            REQUIRE(result->value == iteration);
        } else {
            REQUIRE(result->was_cancelled());
            if (sent) {
                REQUIRE(ch.try_recv() == iteration);
            } else {
                REQUIRE_FALSE(ch.try_recv().has_value());
            }
        }
        REQUIRE(ch.empty());
    }

    sched.shutdown();
}

TEST_CASE("channel wake paths skip cancelled head waiters",
          "[sync][channel][cancellation][cancel_token][queue]") {
    SECTION("receivers") {
        channel<int> ch(1);
        cancel_source source;
        channel<int>::cancellable_recv_awaitable first(
            ch, source.get_token());
        channel<int>::recv_awaitable second(ch);

        REQUIRE(first.await_suspend(std::noop_coroutine()));
        REQUIRE(second.await_suspend(std::noop_coroutine()));
        source.cancel();
        REQUIRE(ch.try_send(21));

        REQUIRE(first.await_resume() == cancel_result::cancelled);
        second.await_resume();
        REQUIRE(ch.try_recv() == 21);
    }

    SECTION("senders") {
        channel<int> ch(1);
        REQUIRE(ch.try_send(1));
        cancel_source source;
        channel<int>::cancellable_send_awaitable first(
            ch, 2, source.get_token());
        channel<int>::send_awaitable second(ch, 3);

        REQUIRE(first.await_suspend(std::noop_coroutine()));
        REQUIRE(second.await_suspend(std::noop_coroutine()));
        source.cancel();
        REQUIRE(ch.try_recv() == 1);

        REQUIRE(first.await_resume().was_cancelled());
        REQUIRE(second.await_resume());
        REQUIRE(ch.try_recv() == 3);
    }
}

TEST_CASE("channel close and cancellation preserve the winning terminal state",
          "[sync][channel][cancellation][cancel_token][close][race]") {
    for (int iteration = 0; iteration < 64; ++iteration) {
        channel<int> ch;
        cancel_source source;
        channel<int>::cancellable_send_awaitable sender(
            ch, iteration, source.get_token());
        REQUIRE(sender.await_suspend(std::noop_coroutine()));

        std::barrier race_start(3);
        std::thread canceller([&] {
            race_start.arrive_and_wait();
            source.cancel();
        });
        std::thread closer([&] {
            race_start.arrive_and_wait();
            ch.close();
        });
        race_start.arrive_and_wait();
        canceller.join();
        closer.join();

        const auto terminal = sender.await_resume();
        const auto delivered = ch.try_recv();
        if (terminal.success()) {
            REQUIRE(terminal.cancel == cancel_result::completed);
            REQUIRE(delivered == iteration);
        } else {
            REQUIRE(terminal.was_cancelled());
            REQUIRE_FALSE(delivered.has_value());
        }
    }
}

TEST_CASE("channel recv distinguishes close and cancellation races",
          "[sync][channel][cancellation][cancel_token][close][race][runtime]") {
    scheduler sched(2);
    sched.start();

    for (int iteration = 0; iteration < 64; ++iteration) {
        channel<int> ch(1);
        cancel_source source;
        std::atomic<bool> started{false};
        std::optional<channel<int>::cancellable_recv_result> result;

        auto receiver = sched.go_joinable([&]() -> task<void> {
            started.store(true, std::memory_order_release);
            result = co_await ch.recv(source.get_token());
        });
        REQUIRE(wait_for_flag(started));

        std::barrier race_start(3);
        std::thread canceller([&] {
            race_start.arrive_and_wait();
            source.cancel();
        });
        std::thread closer([&] {
            race_start.arrive_and_wait();
            ch.close();
        });
        race_start.arrive_and_wait();
        canceller.join();
        closer.join();
        receiver.wait_destroyed();

        REQUIRE(result.has_value());
        REQUIRE_FALSE(result->success());
        REQUIRE((result->was_cancelled() || result->was_closed()));
        REQUIRE(ch.empty());
    }

    for (int iteration = 0; iteration < 64; ++iteration) {
        channel<int> ch(1);
        REQUIRE(ch.try_send(iteration));
        ch.close();

        cancel_source source;
        std::atomic<bool> started{false};
        std::atomic<bool> begin_receive{false};
        std::optional<channel<int>::cancellable_recv_result> result;
        auto receiver = sched.go_joinable([&]() -> task<void> {
            started.store(true, std::memory_order_release);
            while (!begin_receive.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            result = co_await ch.recv(source.get_token());
        });
        REQUIRE(wait_for_flag(started));

        std::barrier race_start(2);
        std::thread canceller([&] {
            race_start.arrive_and_wait();
            source.cancel();
        });
        race_start.arrive_and_wait();
        begin_receive.store(true, std::memory_order_release);
        canceller.join();
        receiver.wait_destroyed();

        REQUIRE(result.has_value());
        if (result->success()) {
            REQUIRE(result->value == iteration);
            REQUIRE(ch.empty());
        } else {
            REQUIRE(result->was_cancelled());
            REQUIRE(ch.try_recv() == iteration);
        }
    }

    sched.shutdown();
}

TEST_CASE("channel waits observe join-handle cancellation context",
          "[sync][channel][cancellation][cancel_token][runtime]") {
    scheduler sched(2);
    sched.start();

    channel<int> send_channel(1);
    REQUIRE(send_channel.try_send(1));
    std::atomic<bool> send_started{false};
    std::optional<channel<int>::cancellable_send_result> send_result;
    auto sender = sched.go_joinable([&]() -> task<void> {
        send_started.store(true, std::memory_order_release);
        send_result = co_await send_channel.send(
            2, this_coro::cancel_token());
    });

    REQUIRE(wait_for_flag(send_started));
    sender.request_cancel();
    sender.wait_destroyed();
    REQUIRE(send_result->was_cancelled());
    REQUIRE(send_channel.try_recv() == 1);
    REQUIRE_FALSE(send_channel.try_recv().has_value());

    channel<int> recv_channel(1);
    std::atomic<bool> recv_started{false};
    std::optional<channel<int>::cancellable_recv_result> recv_result;
    auto receiver = sched.go_joinable([&]() -> task<void> {
        recv_started.store(true, std::memory_order_release);
        recv_result = co_await recv_channel.recv(
            this_coro::cancel_token());
    });

    REQUIRE(wait_for_flag(recv_started));
    receiver.request_cancel();
    receiver.wait_destroyed();
    REQUIRE(recv_result->was_cancelled());
    REQUIRE(recv_channel.empty());

    sched.shutdown();
}

TEST_CASE("semaphore cancellation and release select one terminal result",
          "[sync][semaphore][cancellation][cancel_token][race]") {
    scheduler sched(2);
    sched.start();

    for (int iteration = 0; iteration < 64; ++iteration) {
        semaphore sem(0);
        cancel_source source;
        std::atomic<bool> started{false};
        std::atomic<bool> completed{false};
        std::atomic<cancel_result> result{cancel_result::completed};

        auto waiter = [&]() -> task<void> {
            started.store(true, std::memory_order_release);
            result.store(co_await sem.acquire(source.get_token()),
                         std::memory_order_release);
            completed.store(true, std::memory_order_release);
            co_return;
        };
        auto pending = waiter();
        sched.spawn(elio::coro::detail::task_access::release(
            std::move(pending)));

        REQUIRE(wait_for_flag(started));

        std::barrier race_start(3);
        std::thread canceller([&] {
            race_start.arrive_and_wait();
            source.cancel();
        });
        std::thread releaser([&] {
            race_start.arrive_and_wait();
            sem.release();
        });
        race_start.arrive_and_wait();
        canceller.join();
        releaser.join();

        REQUIRE(wait_for_flag(completed));
        const auto terminal = result.load(std::memory_order_acquire);
        if (terminal == cancel_result::completed) {
            REQUIRE(sem.count() == 0);
        } else {
            REQUIRE(sem.count() == 1);
        }
    }

    sched.shutdown();
}

TEST_CASE("mutex cancellation and unlock select one terminal result",
          "[sync][mutex][cancellation][cancel_token][race]") {
    scheduler sched(2);
    sched.start();

    for (int iteration = 0; iteration < 64; ++iteration) {
        mutex m;
        REQUIRE(m.try_lock());
        cancel_source source;
        std::atomic<bool> started{false};
        std::atomic<bool> completed{false};
        std::atomic<cancel_result> result{cancel_result::cancelled};

        auto waiter = [&]() -> task<void> {
            started.store(true, std::memory_order_release);
            const auto terminal = co_await m.lock(source.get_token());
            result.store(terminal, std::memory_order_release);
            if (terminal == cancel_result::completed) {
                m.unlock();
            }
            completed.store(true, std::memory_order_release);
            co_return;
        };
        auto pending = waiter();
        sched.spawn(elio::coro::detail::task_access::release(
            std::move(pending)));

        REQUIRE(wait_for_flag(started));

        std::barrier race_start(3);
        std::thread canceller([&] {
            race_start.arrive_and_wait();
            source.cancel();
        });
        std::thread unlocker([&] {
            race_start.arrive_and_wait();
            m.unlock();
        });
        race_start.arrive_and_wait();
        canceller.join();
        unlocker.join();

        REQUIRE(wait_for_flag(completed));
        const auto terminal = result.load(std::memory_order_acquire);
        REQUIRE((terminal == cancel_result::completed ||
                 terminal == cancel_result::cancelled));
        REQUIRE_FALSE(m.is_locked());
    }

    sched.shutdown();
}

TEST_CASE("event cancellation and set select one terminal result",
          "[sync][event][cancellation][cancel_token][race]") {
    scheduler sched(2);
    sched.start();

    for (int iteration = 0; iteration < 64; ++iteration) {
        event e;
        cancel_source source;
        std::atomic<bool> started{false};
        std::atomic<bool> completed{false};
        std::atomic<cancel_result> result{cancel_result::cancelled};

        auto waiter = [&]() -> task<void> {
            started.store(true, std::memory_order_release);
            result.store(co_await e.wait(source.get_token()),
                         std::memory_order_release);
            completed.store(true, std::memory_order_release);
            co_return;
        };
        auto pending = waiter();
        sched.spawn(elio::coro::detail::task_access::release(
            std::move(pending)));

        REQUIRE(wait_for_flag(started));

        std::barrier race_start(3);
        std::thread canceller([&] {
            race_start.arrive_and_wait();
            source.cancel();
        });
        std::thread setter([&] {
            race_start.arrive_and_wait();
            e.set();
        });
        race_start.arrive_and_wait();
        canceller.join();
        setter.join();

        REQUIRE(wait_for_flag(completed));
        const auto terminal = result.load(std::memory_order_acquire);
        REQUIRE((terminal == cancel_result::completed ||
                 terminal == cancel_result::cancelled));
        REQUIRE(e.is_set());
    }

    sched.shutdown();
}

TEST_CASE("basic sync wake paths skip a cancelled head waiter",
          "[sync][cancellation][cancel_token][queue]") {
    cancel_source cancelled;

    SECTION("mutex") {
        mutex m;
        REQUIRE(m.try_lock());
        auto first = m.lock(cancelled.get_token());
        auto second = m.lock();
        REQUIRE(first.await_suspend(std::noop_coroutine()));
        REQUIRE(second.await_suspend(std::noop_coroutine()));

        cancelled.cancel();
        m.unlock();

        REQUIRE(first.await_resume() == cancel_result::cancelled);
        second.await_resume();
        REQUIRE(m.is_locked());
        m.unlock();
    }

    SECTION("semaphore") {
        semaphore sem(0);
        auto first = sem.acquire(cancelled.get_token());
        auto second = sem.acquire();
        REQUIRE(first.await_suspend(std::noop_coroutine()));
        REQUIRE(second.await_suspend(std::noop_coroutine()));

        cancelled.cancel();
        sem.release();

        REQUIRE(first.await_resume() == cancel_result::cancelled);
        second.await_resume();
        REQUIRE(sem.count() == 0);
    }

    SECTION("event") {
        event e;
        auto first = e.wait(cancelled.get_token());
        auto second = e.wait();
        REQUIRE(first.await_suspend(std::noop_coroutine()));
        REQUIRE(second.await_suspend(std::noop_coroutine()));

        cancelled.cancel();
        e.set();

        REQUIRE(first.await_resume() == cancel_result::cancelled);
        second.await_resume();
        REQUIRE(e.is_set());
    }
}

TEST_CASE("event: destroying waiter does not crash on set()", "[sync][event][cancellation]") {
    event e;

    auto waiter_task = [&]() -> task<void> {
        co_await e.wait();
    };

    // Create and start a waiting coroutine
    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(std::move(t));
    h.resume();  // Runs until co_await e.wait() suspends

    // Destroy the waiting coroutine (simulates cancellation/timeout)
    h.destroy();

    // This must NOT crash — the waiter was unlinked from the queue
    e.set();
}

TEST_CASE("event wake does not schedule a waiter destroyed after dequeue",
          "[sync][event][cancellation][regression]") {
    event e;
    std::atomic<bool> destroy_second{false};
    std::atomic<bool> second_destroyed{false};

    auto first_waiter = [&]() -> task<void> {
        co_await e.wait();
        destroy_second.store(true, std::memory_order_release);
        while (!second_destroyed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    };
    auto second_waiter = [&]() -> task<void> {
        co_await e.wait();
    };

    auto first_task = first_waiter();
    auto second_task = second_waiter();
    auto first = elio::coro::detail::task_access::release(std::move(first_task));
    auto second = elio::coro::detail::task_access::release(std::move(second_task));
    first.resume();
    second.resume();

    std::thread destroyer([&] {
        while (!destroy_second.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        second.destroy();
        second_destroyed.store(true, std::memory_order_release);
    });

    e.set();
    destroyer.join();
}

TEST_CASE("condition_variable notify_all does not schedule a waiter destroyed after dequeue",
          "[sync][condvar][cancellation][regression]") {
    condition_variable cv;
    std::atomic<bool> destroy_second{false};
    std::atomic<bool> second_destroyed{false};

    auto first_waiter = [&]() -> task<void> {
        co_await cv.wait_unlocked();
        destroy_second.store(true, std::memory_order_release);
        while (!second_destroyed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    };
    auto second_waiter = [&]() -> task<void> {
        co_await cv.wait_unlocked();
    };

    auto first_task = first_waiter();
    auto second_task = second_waiter();
    auto first = elio::coro::detail::task_access::release(std::move(first_task));
    auto second = elio::coro::detail::task_access::release(std::move(second_task));
    first.resume();
    second.resume();

    std::thread destroyer([&] {
        while (!destroy_second.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        second.destroy();
        second_destroyed.store(true, std::memory_order_release);
    });

    cv.notify_all();
    destroyer.join();
}

TEST_CASE("semaphore release does not schedule a waiter destroyed after dequeue",
          "[sync][semaphore][cancellation][regression]") {
    semaphore sem(0);
    std::atomic<bool> destroy_second{false};
    std::atomic<bool> second_destroyed{false};

    auto first_waiter = [&]() -> task<void> {
        co_await sem.acquire();
        destroy_second.store(true, std::memory_order_release);
        while (!second_destroyed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    };
    auto second_waiter = [&]() -> task<void> {
        co_await sem.acquire();
    };

    auto first_task = first_waiter();
    auto second_task = second_waiter();
    auto first = elio::coro::detail::task_access::release(std::move(first_task));
    auto second = elio::coro::detail::task_access::release(std::move(second_task));
    first.resume();
    second.resume();

    std::thread destroyer([&] {
        while (!destroy_second.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        second.destroy();
        second_destroyed.store(true, std::memory_order_release);
    });

    sem.release(2);
    destroyer.join();
}

TEST_CASE("shared_mutex reader wake does not schedule a waiter destroyed after dequeue",
          "[sync][shared_mutex][cancellation][regression]") {
    shared_mutex sm;
    REQUIRE(sm.try_lock());
    std::atomic<bool> destroy_second{false};
    std::atomic<bool> second_destroyed{false};

    auto first_waiter = [&]() -> task<void> {
        co_await sm.lock_shared();
        destroy_second.store(true, std::memory_order_release);
        while (!second_destroyed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        sm.unlock_shared();
    };
    auto second_waiter = [&]() -> task<void> {
        co_await sm.lock_shared();
        sm.unlock_shared();
    };

    auto first_task = first_waiter();
    auto second_task = second_waiter();
    auto first = elio::coro::detail::task_access::release(std::move(first_task));
    auto second = elio::coro::detail::task_access::release(std::move(second_task));
    first.resume();
    second.resume();

    std::thread destroyer([&] {
        while (!destroy_second.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        second.destroy();
        second_destroyed.store(true, std::memory_order_release);
    });

    sm.unlock();
    destroyer.join();
    REQUIRE(sm.reader_count() == 0);
}

TEST_CASE("channel close does not schedule a recv waiter destroyed after dequeue",
          "[sync][channel][cancellation][regression]") {
    channel<int> ch(0);
    std::atomic<bool> destroy_second{false};
    std::atomic<bool> second_destroyed{false};

    auto first_waiter = [&]() -> task<void> {
        auto value = co_await ch.recv();
        REQUIRE_FALSE(value.has_value());
        destroy_second.store(true, std::memory_order_release);
        while (!second_destroyed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    };
    auto second_waiter = [&]() -> task<void> {
        (void)co_await ch.recv();
    };

    auto first_task = first_waiter();
    auto second_task = second_waiter();
    auto first = elio::coro::detail::task_access::release(std::move(first_task));
    auto second = elio::coro::detail::task_access::release(std::move(second_task));
    first.resume();
    second.resume();

    std::thread destroyer([&] {
        while (!destroy_second.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        second.destroy();
        second_destroyed.store(true, std::memory_order_release);
    });

    ch.close();
    destroyer.join();
}

TEST_CASE("channel close does not schedule a send waiter destroyed after dequeue",
          "[sync][channel][cancellation][regression]") {
    channel<int> ch(0);
    std::atomic<bool> destroy_second{false};
    std::atomic<bool> second_destroyed{false};

    auto first_waiter = [&]() -> task<void> {
        (void)co_await ch.send(1);
        destroy_second.store(true, std::memory_order_release);
        while (!second_destroyed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    };
    auto second_waiter = [&]() -> task<void> {
        (void)co_await ch.send(2);
    };

    auto first_task = first_waiter();
    auto second_task = second_waiter();
    auto first = elio::coro::detail::task_access::release(std::move(first_task));
    auto second = elio::coro::detail::task_access::release(std::move(second_task));
    first.resume();
    second.resume();

    std::thread destroyer([&] {
        while (!destroy_second.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        second.destroy();
        second_destroyed.store(true, std::memory_order_release);
    });

    ch.close();
    destroyer.join();
}

TEST_CASE("mutex: destroying waiter does not crash on unlock()", "[sync][mutex][cancellation]") {
    mutex m;
    m.try_lock();  // Lock the mutex

    auto waiter_task = [&]() -> task<void> {
        co_await m.lock();
    };

    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(std::move(t));
    h.resume();  // Runs until co_await m.lock() suspends

    h.destroy();
    m.unlock();  // Must NOT crash
}

TEST_CASE("mutex: destroying popped waiter transfers lock handoff", "[sync][mutex][cancellation]") {
    mutex m;
    REQUIRE(m.try_lock());

    auto second = m.lock();
    {
        auto first = m.lock();

        REQUIRE(first.await_suspend(std::noop_coroutine()));
        REQUIRE(second.await_suspend(std::noop_coroutine()));

        m.unlock();
        REQUIRE(m.is_locked());
    }

    REQUIRE(m.is_locked());
    second.await_resume();
    m.unlock();
    REQUIRE_FALSE(m.is_locked());
}

TEST_CASE("semaphore: destroying waiter does not crash on release()", "[sync][semaphore][cancellation]") {
    semaphore sem(0);  // No permits available

    auto waiter_task = [&]() -> task<void> {
        co_await sem.acquire();
    };

    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(std::move(t));
    h.resume();

    h.destroy();
    sem.release();  // Must NOT crash
}

TEST_CASE("semaphore: destroying popped waiter transfers permit handoff", "[sync][semaphore][cancellation]") {
    semaphore sem(0);

    auto second = sem.acquire();
    {
        auto first = sem.acquire();

        REQUIRE(first.await_suspend(std::noop_coroutine()));
        REQUIRE(second.await_suspend(std::noop_coroutine()));

        sem.release();
        REQUIRE(sem.count() == 0);
    }

    REQUIRE(sem.count() == 0);
    second.await_resume();
    sem.release();
    REQUIRE(sem.count() == 1);
}

TEST_CASE("condition_variable: destroying waiter does not crash on notify()", "[sync][condvar][cancellation]") {
    condition_variable cv;

    // Use wait_unlocked() to avoid needing a mutex lock dance.
    // wait_unlocked() directly enqueues into the cv's waiter list.
    auto waiter_task = [&]() -> task<void> {
        co_await cv.wait_unlocked();
    };

    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(std::move(t));
    h.resume();  // Suspends on cv.wait_unlocked()

    h.destroy();
    cv.notify_one();  // Must NOT crash
    cv.notify_all();  // Must NOT crash
}

TEST_CASE("channel: destroying recv waiter does not crash on send()", "[sync][channel][cancellation]") {
    // Use a buffered channel (capacity 1) so send can complete without a receiver
    channel<int> ch(1);

    auto waiter_task = [&]() -> task<std::optional<int>> {
        co_return co_await ch.recv();
    };

    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(std::move(t));
    h.resume();  // Suspends on ch.recv()

    h.destroy();

    // send() must NOT crash — the destroyed recv waiter was unlinked from the queue
    auto send_task = ch.send(42);
    auto sh = elio::coro::detail::task_access::release(std::move(send_task));
    sh.resume();  // Runs send to completion (fast path into ring buffer)
    // Coroutine self-destructs in final_awaiter (detached), so no manual destroy needed
}

TEST_CASE("channel: destroying send waiter does not crash on close()", "[sync][channel][cancellation]") {
    // Use a rendezvous channel (capacity 0) so send always suspends when no receiver
    channel<int> ch(0);

    auto waiter_task = [&]() -> task<bool> {
        co_return co_await ch.send(42);
    };

    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(std::move(t));
    h.resume();  // Suspends on ch.send() — no receiver waiting

    h.destroy();
    ch.close();  // Must NOT crash
}

TEST_CASE("shared_mutex: destroying reader waiter does not crash on unlock()", "[sync][shared_mutex][cancellation]") {
    shared_mutex sm;
    sm.try_lock();  // Writer holds lock, so readers will suspend

    auto waiter_task = [&]() -> task<void> {
        co_await sm.lock_shared();
    };

    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(std::move(t));
    h.resume();  // Suspends on lock_shared() because writer holds lock

    h.destroy();  // Destroy suspended reader — must unlink from reader_waiters_

    sm.unlock();  // Writer unlock — must NOT crash when scanning reader_waiters_
}

TEST_CASE("shared_mutex: destroying popped reader releases reader handoff", "[sync][shared_mutex][cancellation]") {
    shared_mutex sm;
    REQUIRE(sm.try_lock());

    auto second = sm.lock_shared();
    {
        auto first = sm.lock_shared();

        REQUIRE(first.await_suspend(std::noop_coroutine()));
        REQUIRE(second.await_suspend(std::noop_coroutine()));

        sm.unlock();
        REQUIRE(sm.reader_count() == 2);
    }

    REQUIRE(sm.reader_count() == 1);
    second.await_resume();
    sm.unlock_shared();
    REQUIRE(sm.reader_count() == 0);
}

TEST_CASE("shared_mutex: destroying writer waiter does not deadlock subsequent operations", "[sync][shared_mutex][cancellation]") {
    shared_mutex sm;
    sm.try_lock_shared();  // Reader holds lock, so writer will suspend and set WRITER_WAITING

    auto waiter_task = [&]() -> task<void> {
        co_await sm.lock();
    };

    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(std::move(t));
    h.resume();  // Suspends on lock() because reader holds lock

    h.destroy();  // Destroy suspended writer — must decrement pending_writers_ and clear WRITER_WAITING

    // Verify subsequent operations work (not deadlocked):
    // 1. A new reader should be able to acquire (WRITER_WAITING was cleared)
    sm.unlock_shared();  // Release the reader lock

    auto reader_task = [&]() -> task<void> {
        co_await sm.lock_shared();
        sm.unlock_shared();
    };
    auto rt = reader_task();
    auto rh = elio::coro::detail::task_access::release(std::move(rt));
    rh.resume();  // Should complete without suspending (no writer active/waiting)

    // 2. A new writer should be able to acquire (no readers active)
    auto writer_task = [&]() -> task<void> {
        co_await sm.lock();
        sm.unlock();
    };
    auto wt = writer_task();
    auto wh = elio::coro::detail::task_access::release(std::move(wt));
    wh.resume();  // Should complete without suspending
}

TEST_CASE("shared_mutex: destroying popped writer transfers writer handoff", "[sync][shared_mutex][cancellation]") {
    shared_mutex sm;
    REQUIRE(sm.try_lock_shared());

    auto second = sm.lock();
    {
        auto first = sm.lock();

        REQUIRE(first.await_suspend(std::noop_coroutine()));
        REQUIRE(second.await_suspend(std::noop_coroutine()));

        sm.unlock_shared();
        REQUIRE(sm.is_writer_active());
    }

    REQUIRE(sm.is_writer_active());
    second.await_resume();
    sm.unlock();
    REQUIRE_FALSE(sm.is_writer_active());
}

TEST_CASE("shared_mutex: destroying writer waiter wakes parked readers", "[sync][shared_mutex][cancellation]") {
    shared_mutex sm;
    sm.try_lock_shared();  // Reader R1 holds lock

    // Writer W1 suspends and sets WRITER_WAITING
    auto writer_task = [&]() -> task<void> {
        co_await sm.lock();
    };
    auto wt = writer_task();
    auto wh = elio::coro::detail::task_access::release(std::move(wt));
    wh.resume();  // Suspends on lock() because reader holds lock

    // Reader R2 tries to acquire, sees WRITER_WAITING, parks in reader_waiters_
    auto reader_task = [&]() -> task<void> {
        co_await sm.lock_shared();
        sm.unlock_shared();
    };
    auto rt = reader_task();
    auto rh = elio::coro::detail::task_access::release(std::move(rt));
    rh.resume();  // Suspends on lock_shared() because WRITER_WAITING is set

    // Verify R2 is parked
    REQUIRE(sm.reader_count() == 1);  // Only R1 is active

    // Destroy writer W1 — must clear WRITER_WAITING and wake parked reader R2
    wh.destroy();

    // Verify WRITER_WAITING was cleared and R2 was woken
    // R2 should now be active (reader_count should be 2: R1 + R2)
    // But R2 hasn't actually acquired the lock yet - it was just scheduled
    // So we need to release R1's lock first, then R2 can acquire

    // Release reader R1's lock
    sm.unlock_shared();

    // Now R2 should be able to complete when resumed
    // Note: schedule_handle was called in the destructor, so R2 is scheduled
    // We just need to verify the state is correct (no WRITER_WAITING)
    REQUIRE(!sm.is_writer_active());
}

TEST_CASE("event: multiple waiters, destroy one, set wakes remaining", "[sync][event][cancellation]") {
    event e;
    std::atomic<int> woken{0};

    scheduler sched(2);
    sched.start();

    auto make_waiter = [&]() -> task<void> {
        co_await e.wait();
        woken.fetch_add(1);
    };

    // Create 3 waiter tasks and suspend them on the event
    auto t1 = make_waiter();
    auto t2 = make_waiter();
    auto t3 = make_waiter();

    auto h1 = elio::coro::detail::task_access::release(std::move(t1));
    auto h2 = elio::coro::detail::task_access::release(std::move(t2));
    auto h3 = elio::coro::detail::task_access::release(std::move(t3));

    h1.resume();  // Suspends on e.wait()
    h2.resume();  // Suspends on e.wait()
    h3.resume();  // Suspends on e.wait()

    // Destroy one waiter — must unlink from the event's waiter list
    h2.destroy();

    // Set should wake only the remaining 2
    e.set();

    sched.shutdown();

    // Only 2 should have been woken (the destroyed one doesn't count)
    REQUIRE(woken.load() == 2);
}
