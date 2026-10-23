#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/runtime/worker_lease.hpp>
#include <elio/coro/task_group.hpp>
#include <elio/coro/generator.hpp>
#include <elio/runtime/affinity.hpp>
#include <elio/sync/event.hpp>
#include <elio/time/timer.hpp>
#include <atomic>
#include <barrier>
#include <chrono>
#include <optional>
#include <thread>
#include <type_traits>
#include "../test_main.cpp"

using namespace elio;
using namespace elio::runtime;
using namespace std::chrono_literals;

namespace {

template<typename Predicate>
bool eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + test::scaled_sec(3);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return predicate();
}

struct backend_selection {
    explicit backend_selection(io::io_context::backend_type backend)
        : previous(runtime::detail::worker_io_backend_for_test.exchange(backend)) {}
    ~backend_selection() {
        runtime::detail::worker_io_backend_for_test.store(previous);
    }
    io::io_context::backend_type previous;
};

} // namespace

static_assert(!std::is_copy_constructible_v<external_worker_lease>);
static_assert(std::is_nothrow_move_constructible_v<external_worker_lease>);
static_assert(std::is_nothrow_move_assignable_v<external_worker_lease>);

TEST_CASE("worker lease retains an event waiter through shrink", "[worker_lease][scheduler]") {
    const auto selected = GENERATE(io::io_context::backend_type::epoll,
                                  io::io_context::backend_type::auto_detect,
                                  io::io_context::backend_type::io_uring);
    backend_selection backend(selected);
    std::optional<scheduler> storage;
    try {
        storage.emplace(2);
    } catch (const std::runtime_error& error) {
        if (selected == io::io_context::backend_type::io_uring) {
            SKIP("Explicit io_uring is unavailable: " << error.what());
        }
        throw;
    }
    auto& sched = *storage;
    auto* owner = sched.get_worker(1);
    sync::event gate;
    std::atomic<bool> acquired{false}, finished{false}, correct_owner{false};
    std::atomic<bool> move_valid{false};
    sched.start();
    sched.go_to(1, [&]() -> coro::task<void> {
        auto first = co_await lease_current_worker();
        auto second = co_await lease_current_worker();
        auto third = co_await lease_current_worker();
        const auto generation = first.context_generation();
        external_worker_lease moved(std::move(first));
        first = std::move(second);
        moved = std::move(third);
        move_valid.store(!second.active() && moved.active() && first.active() &&
                         owner->io_context().active_pin_count() == 2);
        acquired.store(true, std::memory_order_release);
        co_await gate.wait();
        correct_owner.store(worker_thread::current() == owner &&
                            moved.context_generation() == generation);
        moved.release();
        moved.release();
        first.release();
        finished.store(true, std::memory_order_release);
    });
    const bool started = eventually([&] { return acquired.load(std::memory_order_acquire); });
    sched.set_thread_count(1);
    const bool retained = owner->is_running() && sched.pending_tasks() >= 2 &&
                          owner->io_context().active_pin_count() == 2;
    gate.set();
    const bool completed = eventually([&] { return finished.load(std::memory_order_acquire); });
    const bool retired = eventually([&] { return !owner->is_running(); });
    // Reuse must wait for the old OS thread and must not inherit its pins.
    sched.set_thread_count(2);
    const bool reused_cleanly = owner->is_running() &&
                               owner->io_context().active_pin_count() == 0;
    sched.shutdown();
    REQUIRE(started);
    REQUIRE(retained);
    REQUIRE(completed);
    REQUIRE(retired);
    REQUIRE(correct_owner.load());
    REQUIRE(move_valid.load());
    REQUIRE(reused_cleanly);
}

TEST_CASE("worker lease acquisition rejects after shrink admission closes", "[worker_lease][scheduler]") {
    backend_selection backend(io::io_context::backend_type::epoll);
    scheduler sched(2);
    auto* owner = sched.get_worker(1);
    std::atomic<bool> entered{false}, proceed{false}, finished{false}, rejected{false};
    sched.start();
    sched.go_to(1, [&]() -> coro::task<void> {
        entered.store(true, std::memory_order_release);
        // A deterministic test barrier, not an application worker wait.
        proceed.wait(false, std::memory_order_acquire);
        try {
            auto lease = co_await lease_current_worker();
        } catch (const std::runtime_error&) {
            rejected.store(true);
        }
        finished.store(true, std::memory_order_release);
    });
    const bool started = eventually([&] { return entered.load(std::memory_order_acquire); });
    sched.set_thread_count(1);
    const bool draining = owner->is_draining();
    proceed.store(true, std::memory_order_release);
    proceed.notify_all();
    const bool completed = eventually([&] { return finished.load(std::memory_order_acquire); });
    sched.shutdown();
    REQUIRE(started);
    REQUIRE(draining);
    REQUIRE(completed);
    REQUIRE(rejected.load());
    REQUIRE(owner->io_context().active_pin_count() == 0);
}

TEST_CASE("yielding leased owner cannot monopolize retirement service", "[worker_lease][scheduler]") {
    backend_selection backend(io::io_context::backend_type::epoll);
    scheduler sched(2);
    auto* owner = sched.get_worker(1);
    std::atomic<bool> acquired{false}, proceed{false}, finished{false};
    std::atomic<size_t> polls{0};
    sched.start();
    sched.go_to(1, [&]() -> coro::task<void> {
        auto lease = co_await lease_current_worker();
        acquired.store(true, std::memory_order_release);
        proceed.wait(false, std::memory_order_acquire);
        const auto before = owner->blocking_poll_calls_for_test();
        for (size_t i = 0; i < 4096; ++i) co_await time::yield();
        polls.store(owner->blocking_poll_calls_for_test() - before);
        lease.release();
        finished.store(true, std::memory_order_release);
    });
    const bool started = eventually([&] { return acquired.load(std::memory_order_acquire); });
    sched.set_thread_count(1);
    proceed.store(true, std::memory_order_release);
    proceed.notify_all();
    const bool completed = eventually([&] { return finished.load(std::memory_order_acquire); });
    sched.shutdown();
    REQUIRE(started);
    REQUIRE(completed);
    REQUIRE(polls.load() >= 15);
}

TEST_CASE("foreign lease release survives forced scheduler destruction", "[worker_lease][scheduler]") {
    backend_selection backend(io::io_context::backend_type::epoll);
    std::optional<external_worker_lease> retained;
    std::atomic<bool> acquired{false};
    {
        scheduler sched(1);
        sched.start();
        sched.go([&]() -> coro::task<void> {
            retained.emplace(co_await lease_current_worker());
            acquired.store(true, std::memory_order_release);
            co_return;
        });
        const bool started = eventually([&] { return acquired.load(std::memory_order_acquire); });
        if (!started) {
            sched.shutdown_force();
            FAIL("lease acquisition did not run");
        }
        REQUIRE(sched.pending_tasks() >= 1);
        sched.shutdown_force();
    }
    REQUIRE(retained->active());
    retained->release();
    retained->release();
    REQUIRE_FALSE(retained->active());
}

TEST_CASE("foreign release wakes a draining worker", "[worker_lease][scheduler]") {
    backend_selection backend(io::io_context::backend_type::epoll);
    scheduler sched(2);
    auto* owner = sched.get_worker(1);
    std::optional<external_worker_lease> retained;
    std::atomic<bool> acquired{false};
    sched.start();
    sched.go_to(1, [&]() -> coro::task<void> {
        retained.emplace(co_await lease_current_worker());
        acquired.store(true, std::memory_order_release);
        co_return;
    });
    const bool started = eventually([&] { return acquired.load(std::memory_order_acquire); });
    sched.set_thread_count(1);
    const bool retained_owner = owner->is_running();
    if (started) retained->release();
    const bool retired = eventually([&] { return !owner->is_running(); });
    sched.shutdown();
    REQUIRE(started);
    REQUIRE(retained_owner);
    REQUIRE(retired);
}

TEST_CASE("graceful shutdown waits for independent lease release", "[worker_lease][scheduler]") {
    backend_selection backend(io::io_context::backend_type::epoll);
    scheduler sched(1);
    std::optional<external_worker_lease> retained;
    std::atomic<bool> acquired{false}, shutdown_returned{false};
    bool drained = false;
    sched.start();
    sched.go([&]() -> coro::task<void> {
        retained.emplace(co_await lease_current_worker());
        acquired.store(true, std::memory_order_release);
        co_return;
    });
    const bool started = eventually([&] { return acquired.load(std::memory_order_acquire); });
    runtime::detail::graceful_admission_closed_for_test.store(false);
    std::thread shutdown([&] {
        drained = sched.shutdown(test::scaled_sec(3));
        shutdown_returned.store(true, std::memory_order_release);
    });
    const bool closed = eventually([] {
        return runtime::detail::graceful_admission_closed_for_test.load(std::memory_order_acquire);
    });
    const bool waited = !shutdown_returned.load(std::memory_order_acquire);
    if (started) retained->release();
    shutdown.join();
    REQUIRE(started);
    REQUIRE(closed);
    REQUIRE(waited);
    REQUIRE(drained);
}

namespace {
coro::generator<int> gated_producer(sync::event* gate, bool yield_value) {
    co_await gate->wait();
    if (yield_value) co_yield 42;
}

coro::task<void> acquire_without_worker(bool* rejected) {
    try {
        auto lease = co_await lease_current_worker();
    } catch (const std::logic_error&) {
        *rejected = true;
    }
}

coro::task<void> throwing_lease_child() {
    auto lease = co_await lease_current_worker();
    throw std::runtime_error("owner setup failed");
}
} // namespace

TEST_CASE("worker lease rejects off-worker acquisition", "[worker_lease][scheduler]") {
    bool rejected = false;
    auto value = acquire_without_worker(&rejected);
    coro::detail::task_access::handle(value).resume();
    REQUIRE(rejected);
}

TEST_CASE("nested lease setup failure unwinds its pin", "[worker_lease][scheduler]") {
    backend_selection backend(io::io_context::backend_type::epoll);
    scheduler sched(1);
    std::atomic<bool> finished{false}, released{false};
    sched.start();
    sched.go([&]() -> coro::task<void> {
        try {
            co_await throwing_lease_child();
        } catch (const std::runtime_error&) {
            released.store(worker_thread::current()->io_context().active_pin_count() == 0);
        }
        finished.store(true, std::memory_order_release);
    });
    const bool completed = eventually([&] { return finished.load(std::memory_order_acquire); });
    sched.shutdown();
    REQUIRE(completed);
    REQUIRE(released.load());
}

TEST_CASE("stale lease release cannot affect another scheduler generation", "[worker_lease][scheduler]") {
    backend_selection backend(io::io_context::backend_type::epoll);
    std::optional<external_worker_lease> stale, current;
    std::atomic<bool> acquired{false};
    {
        scheduler old(1);
        old.start();
        old.go([&]() -> coro::task<void> {
            stale.emplace(co_await lease_current_worker());
            acquired.store(true, std::memory_order_release);
            co_return;
        });
        const bool started = eventually([&] { return acquired.load(std::memory_order_acquire); });
        old.shutdown_force();
        REQUIRE(started);
    }
    scheduler replacement(1);
    acquired.store(false);
    replacement.start();
    replacement.go([&]() -> coro::task<void> {
        current.emplace(co_await lease_current_worker());
        acquired.store(true, std::memory_order_release);
        co_return;
    });
    const bool started = eventually([&] { return acquired.load(std::memory_order_acquire); });
    stale->release();
    const bool unchanged = replacement.get_worker(0)->io_context().active_pin_count() == 1;
    const bool distinct = started && stale->context_generation() != current->context_generation();
    if (started) current->release();
    replacement.shutdown();
    REQUIRE(started);
    REQUIRE(unchanged);
    REQUIRE(distinct);
}

TEST_CASE("foreign notifier preserves a leased event waiter's exact owner", "[worker_lease][scheduler]") {
    const bool different_scheduler = GENERATE(false, true);
    backend_selection backend(io::io_context::backend_type::epoll);
    scheduler sched(2);
    sync::event gate;
    std::atomic<bool> acquired{false}, finished{false}, correct{false};
    sched.start();
    sched.go_to(1, [&]() -> coro::task<void> {
        auto lease = co_await lease_current_worker();
        const auto owner = std::this_thread::get_id();
        acquired.store(true, std::memory_order_release);
        co_await gate.wait();
        correct.store(std::this_thread::get_id() == owner);
        lease.release();
        finished.store(true, std::memory_order_release);
    });
    const bool started = eventually([&] { return acquired.load(std::memory_order_acquire); });
    // Ensure wait() has actually suspended, rather than taking its ready path.
    const bool suspended = eventually([&] { return sched.get_worker(1)->is_idle(); });
    sched.set_thread_count(1);
    if (different_scheduler) {
        scheduler notifier(1);
        notifier.start();
        notifier.go([&]() -> coro::task<void> { gate.set(); co_return; });
        notifier.shutdown();
    } else {
        std::thread notifier([&] { gate.set(); });
        notifier.join();
    }
    const bool completed = eventually([&] { return finished.load(std::memory_order_acquire); });
    if (completed) sched.shutdown();
    else sched.shutdown_force();
    REQUIRE(started);
    REQUIRE(suspended);
    REQUIRE(completed);
    REQUIRE(correct.load());
}

TEST_CASE("foreign cancellation preserves a leased waiter's owner", "[worker_lease][scheduler]") {
    backend_selection backend(io::io_context::backend_type::epoll);
    scheduler sched(2);
    sync::event gate;
    coro::cancel_source source;
    std::atomic<bool> acquired{false}, finished{false}, correct{false};
    sched.start();
    sched.go_to(1, [&]() -> coro::task<void> {
        auto lease = co_await lease_current_worker();
        const auto owner = std::this_thread::get_id();
        acquired.store(true, std::memory_order_release);
        const auto result = co_await gate.wait(source.get_token());
        correct.store(result == coro::cancel_result::cancelled &&
                      std::this_thread::get_id() == owner);
        lease.release();
        finished.store(true, std::memory_order_release);
    });
    const bool started = eventually([&] { return acquired.load(std::memory_order_acquire); });
    const bool suspended = eventually([&] { return sched.get_worker(1)->is_idle(); });
    sched.set_thread_count(1);
    std::thread canceller([&] { source.cancel(); });
    canceller.join();
    const bool completed = eventually([&] { return finished.load(std::memory_order_acquire); });
    if (completed) sched.shutdown();
    else sched.shutdown_force();
    REQUIRE(started);
    REQUIRE(suspended);
    REQUIRE(completed);
    REQUIRE(correct.load());
}

TEST_CASE("foreign lease release races safely with context destruction", "[worker_lease][scheduler]") {
    backend_selection backend(io::io_context::backend_type::epoll);
    for (int iteration = 0; iteration < 16; ++iteration) {
        auto sched = std::make_unique<scheduler>(1);
        std::optional<external_worker_lease> retained;
        std::atomic<bool> acquired{false};
        sched->start();
        sched->go([&]() -> coro::task<void> {
            retained.emplace(co_await lease_current_worker());
            acquired.store(true, std::memory_order_release);
            co_return;
        });
        const bool started = eventually([&] { return acquired.load(std::memory_order_acquire); });
        if (!started) { sched->shutdown_force(); FAIL("lease acquisition did not run"); }
        std::barrier start(2);
        std::thread releaser([&] { start.arrive_and_wait(); retained->release(); });
        start.arrive_and_wait();
        sched.reset();
        releaser.join();
        REQUIRE_FALSE(retained->active());
    }
}

TEST_CASE("isolated scope returns a leased parent to its owner", "[worker_lease][scheduler]") {
    backend_selection backend(io::io_context::backend_type::epoll);
    scheduler sched(2);
    std::atomic<bool> finished{false}, correct{false};
    sched.start();
    sched.go_to(1, [&]() -> coro::task<void> {
        auto lease = co_await lease_current_worker();
        const auto owner = std::this_thread::get_id();
        co_await coro::task_scope([](coro::task_group&) -> coro::task<void> {
            co_await runtime::set_affinity(0);
        });
        correct.store(std::this_thread::get_id() == owner);
        lease.release();
        finished.store(true, std::memory_order_release);
    });
    const bool completed = eventually([&] { return finished.load(std::memory_order_acquire); });
    sched.shutdown();
    REQUIRE(completed);
    REQUIRE(correct.load());
}

TEST_CASE("captured owner endpoint falls back after final lease release", "[worker_lease][scheduler]") {
    backend_selection backend(io::io_context::backend_type::epoll);
    scheduler sched(2);
    auto* old_owner = sched.get_worker(1);
    std::optional<external_worker_lease> retained;
    std::shared_ptr<coro::detail::continuation_owner> endpoint;
    std::atomic<void*> suspended{nullptr};
    std::atomic<bool> finished{false};
    struct manual_wait {
        std::atomic<void*>* address;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle) const noexcept {
            address->store(handle.address(), std::memory_order_release);
        }
        void await_resume() const noexcept {}
    };
    sched.start();
    sched.go_to(1, [&]() -> coro::task<void> {
        retained.emplace(co_await lease_current_worker());
        endpoint = coro::promise_base::current_frame()->pinned_continuation_owner();
        co_await manual_wait{&suspended};
        finished.store(true, std::memory_order_release);
    });
    const bool parked = eventually([&] { return suspended.load(std::memory_order_acquire) != nullptr; });
    sched.set_thread_count(1);
    if (parked) retained->release();
    const bool retired = eventually([&] { return !old_owner->is_running(); });
    // Models capture -> final release -> retire -> notifier dispatch, without
    // relying on timing to land between lookup and endpoint invocation.
    if (parked) endpoint->schedule(std::coroutine_handle<>::from_address(suspended.load()));
    const bool completed = eventually([&] { return finished.load(std::memory_order_acquire); });
    if (completed) sched.shutdown();
    else sched.shutdown_force();
    REQUIRE(parked);
    REQUIRE(retired);
    REQUIRE(completed);
}

TEST_CASE("generator yield and exhaustion restore a leased consumer", "[worker_lease][scheduler]") {
    const bool yield_value = GENERATE(false, true);
    const bool for_each = GENERATE(false, true);
    backend_selection backend(io::io_context::backend_type::epoll);
    scheduler sched(2);
    sync::event gate;
    auto producer = gated_producer(&gate, yield_value);
    std::atomic<bool> started{false}, finished{false}, correct{false}, callback_correct{true};
    sched.start();
    sched.go_to(1, [&]() -> coro::task<void> {
        auto lease = co_await lease_current_worker();
        const auto owner = std::this_thread::get_id();
        started.store(true, std::memory_order_release);
        if (for_each) {
            co_await producer.for_each([&](int) {
                callback_correct.store(std::this_thread::get_id() == owner);
            });
        } else {
            (void)co_await producer.next();
        }
        correct.store(std::this_thread::get_id() == owner);
        lease.release();
        finished.store(true, std::memory_order_release);
    });
    const bool entered = eventually([&] { return started.load(std::memory_order_acquire); });
    const bool suspended = eventually([&] { return sched.get_worker(1)->is_idle(); });
    sched.set_thread_count(1);
    gate.set();
    const bool completed = eventually([&] { return finished.load(std::memory_order_acquire); });
    if (completed) sched.shutdown();
    else sched.shutdown_force();
    REQUIRE(entered);
    REQUIRE(suspended);
    REQUIRE(completed);
    REQUIRE(correct.load());
    REQUIRE(callback_correct.load());
}
