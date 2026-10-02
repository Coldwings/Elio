#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/http/client_base.hpp>
#include <elio/coro/detail/completion_waiter.hpp>
#include <elio/runtime/affinity.hpp>
#include <elio/sync/event.hpp>
#include "../test_main.cpp"

#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
using elio::coro::task;
using backend = elio::io::io_context::backend_type;

// Single-owner test phases reuse the runtime's allocation-free completion slot.
// Their external owner lives through the normally joined child frames.
class setup_phase {
    class awaiter {
    public:
        explicit awaiter(setup_phase& phase) noexcept : phase_(phase), waiter_(phase.slot_) {}
        bool await_ready() const noexcept { return phase_.released_.load(std::memory_order_acquire); }
        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            return phase_.slot_.register_waiter(waiter_, handle, [this] { return await_ready(); });
        }
        void await_resume() const noexcept {}
    private:
        setup_phase& phase_;
        elio::coro::detail::completion_waiter waiter_;
    };
public:
    auto wait() noexcept { return awaiter(*this); }
    void set() noexcept {
        released_.store(true, std::memory_order_release);
        auto wake = slot_.take();
        if (auto handle = wake.claim()) elio::runtime::schedule_handle(handle);
    }
private:
    std::atomic<bool> released_{false};
    elio::coro::detail::completion_waiter_slot slot_;
};

struct backend_guard {
    backend previous;
    explicit backend_guard(backend value)
        : previous(elio::runtime::detail::worker_io_backend_for_test.exchange(value)) {}
    ~backend_guard() { elio::runtime::detail::worker_io_backend_for_test.store(previous); }
};

struct setup_probe {
    elio::sync::event hello_received;
    elio::sync::event release_start;
    setup_phase ready_start;
    setup_phase ready_arm;
    elio::coro::cancel_source recovery;
    std::atomic<bool> timer_failed{false};
    std::atomic<bool> peer_received{false};
    std::atomic<bool> done{false};
    std::atomic<bool> before_start{false};
    std::atomic<bool> construction_failed{false};
    std::atomic<bool> tls_entered{false};
    std::atomic<bool> connect_entered{false};
    std::atomic<bool> destruction_observed{false};
    std::atomic<bool> before_arm{false};
    std::mutex barrier_mutex;
    bool barrier_released = false;
    bool delay_arm = false;
    bool recovered = false;
    std::exception_ptr failure;
};

std::atomic<setup_probe*> observed{nullptr};

task<void> pause_before_timer() {
    auto& probe = *observed.load(std::memory_order_acquire);
    probe.before_start.store(true, std::memory_order_release);
    (void)co_await probe.release_start.wait(probe.recovery.get_token());
}

task<void> pause_after_timer() {
    auto& probe = *observed.load(std::memory_order_acquire);
    probe.before_start.store(true, std::memory_order_release);
    co_await probe.ready_start.wait();
}

void fail_timer_construction() {
    observed.load(std::memory_order_acquire)->construction_failed.store(true, std::memory_order_release);
    throw std::bad_alloc();
}

void observe_tls_entry() {
    observed.load(std::memory_order_acquire)->tls_entered.store(true, std::memory_order_release);
}

void observe_connect_entry() {
    observed.load(std::memory_order_acquire)->connect_entered.store(true, std::memory_order_release);
}

void observe_destruction_wait() {
    observed.load(std::memory_order_acquire)->destruction_observed.store(true, std::memory_order_release);
}

task<elio::coro::cancel_result> fail_ready_timer(
        std::chrono::steady_clock::time_point, elio::coro::cancel_token) {
    co_await elio::runtime::set_affinity(1);
    auto& probe = *observed.load(std::memory_order_acquire);
    if (probe.delay_arm) {
        probe.before_arm.store(true, std::memory_order_release);
        co_await probe.ready_arm.wait();
    }
    {
        std::lock_guard lock(probe.barrier_mutex);
        if (!probe.barrier_released)
            elio::coro::detail::pause_before_detached_frame_destroy_for_test.store(true);
        probe.timer_failed.store(true, std::memory_order_release);
    }
    throw std::runtime_error("ready setup timer failed");
    co_return elio::coro::cancel_result::completed;
}

struct ready_guard {
    setup_probe& probe;
    task<void> (*previous_start)();
    void (*previous_connect)();
    void (*previous_observer)();
    explicit ready_guard(setup_probe& observed_probe)
        : probe(observed_probe)
        , previous_start(elio::http::detail::setup_watchdog_after_start_for_test.exchange(pause_after_timer))
        , previous_connect(elio::http::detail::setup_connect_entered_for_test.exchange(observe_connect_entry))
        , previous_observer(elio::coro::detail::join_destroyed_observer_setup_for_test.exchange(
              observe_destruction_wait)) {
        elio::coro::detail::detached_frame_destroy_paused_for_test.store(false);
        elio::coro::detail::pause_before_detached_frame_destroy_for_test.store(false);
    }
    void release() const noexcept {
        {
            std::lock_guard lock(probe.barrier_mutex);
            // Release is terminal, including a controller observation timeout.
            // A late hook must not park a worker after recovery has begun joining.
            probe.barrier_released = true;
            elio::coro::detail::pause_before_detached_frame_destroy_for_test.store(false);
            elio::coro::detail::pause_before_detached_frame_destroy_for_test.notify_all();
        }
        probe.ready_arm.set();
        probe.ready_start.set();
    }
    ~ready_guard() {
        release();
        try { probe.recovery.cancel(); } catch (...) {}
        elio::coro::detail::join_destroyed_observer_setup_for_test.store(previous_observer);
        elio::http::detail::setup_connect_entered_for_test.store(previous_connect);
        elio::http::detail::setup_watchdog_after_start_for_test.store(previous_start);
        elio::coro::detail::detached_frame_destroy_paused_for_test.store(false);
    }
};

struct startup_guard {
    void (*previous_construction)();
    task<void> (*previous_start)();
    void (*previous_tls)();
    void (*previous_connect)();
    explicit startup_guard(bool during_drain)
        : previous_construction(elio::http::detail::setup_watchdog_before_construct_for_test.exchange(
              during_drain ? nullptr : fail_timer_construction))
        , previous_start(elio::http::detail::setup_watchdog_before_start_for_test.exchange(
              during_drain ? pause_before_timer : nullptr))
        , previous_tls(elio::http::detail::tls_setup_entered_for_test.exchange(observe_tls_entry))
        , previous_connect(elio::http::detail::setup_connect_entered_for_test.exchange(observe_connect_entry)) {
        elio::runtime::detail::graceful_admission_closed_for_test.store(false);
    }
    ~startup_guard() {
        elio::http::detail::setup_connect_entered_for_test.store(previous_connect);
        elio::http::detail::tls_setup_entered_for_test.store(previous_tls);
        elio::http::detail::setup_watchdog_before_start_for_test.store(previous_start);
        elio::http::detail::setup_watchdog_before_construct_for_test.store(previous_construction);
        elio::runtime::detail::graceful_admission_closed_for_test.store(false);
    }
};

task<elio::coro::cancel_result> fail_active_timer(
        std::chrono::steady_clock::time_point, elio::coro::cancel_token token) {
    auto& probe = *observed.load(std::memory_order_acquire);
    const auto result = co_await probe.hello_received.wait(std::move(token));
    if (result != elio::coro::cancel_result::completed) co_return result;
    probe.timer_failed.store(true, std::memory_order_release);
    throw std::runtime_error("setup timer failed");
}

struct timer_guard {
    elio::http::detail::setup_watchdog_wait_hook previous;
    setup_probe* previous_probe;
    explicit timer_guard(setup_probe& probe)
        : previous(elio::http::detail::setup_watchdog_wait_for_test.exchange(fail_active_timer))
        , previous_probe(observed.exchange(&probe)) {}
    ~timer_guard() {
        elio::http::detail::setup_watchdog_wait_for_test.store(previous);
        observed.store(previous_probe);
    }
};

template<typename Predicate>
bool observe(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + elio::test::scaled_ms(2000);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

task<void> withhold_tls_reply(elio::net::tcp_listener& listener, setup_probe& probe) {
    const auto token = probe.recovery.get_token();
    auto stream = co_await listener.accept(token);
    if (!stream) co_return;
    std::array<char, 1> hello{};
    if ((co_await stream->read(hello.data(), hello.size(), token)).result > 0) {
        probe.peer_received.store(true, std::memory_order_release);
        probe.hello_received.set();
    }
    elio::sync::event until_stopped;
    (void)co_await until_stopped.wait(token);
}

task<void> exercise_setup(uint16_t port, elio::tls::tls_context& context, setup_probe& probe,
                          bool secure = true) {
    try {
        (void)co_await elio::http::client_connect_result("127.0.0.1", port, secure,
            secure ? &context : nullptr,
            elio::net::default_cached_resolve_options(), false, std::chrono::seconds(30),
            probe.recovery.get_token());
    } catch (...) { probe.failure = std::current_exception(); }
    probe.done.store(true, std::memory_order_release);
}
} // namespace

TEST_CASE("Connector timer failure cancels a pending real TLS handshake",
          "[http][setup][watchdog][setup-timer-failure][issue-1287]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto workers = GENERATE(size_t{1}, size_t{2});
    CAPTURE(static_cast<int>(selected), workers);
#if ELIO_HAS_IO_URING
    if (selected == backend::io_uring && !elio::io::io_uring_backend::is_available())
        SKIP("io_uring unavailable on this host");
#else
    if (selected == backend::io_uring) SKIP("io_uring support is not compiled");
#endif
    backend_guard backend_scope(selected);
    auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    elio::tls::tls_context context(elio::tls::tls_mode::client);
    setup_probe probe;
    timer_guard timer(probe);
    elio::runtime::scheduler scheduler(workers);
    scheduler.start();
    auto peer = scheduler.go_joinable(withhold_tls_reply(*listener, probe));
    auto operation = scheduler.go_joinable(exercise_setup(listener->local_address().port(), context, probe));
    const bool failed = observe([&] { return probe.timer_failed.load(std::memory_order_acquire); });
    const bool completed = observe([&] { return probe.done.load(std::memory_order_acquire); });
    if (!completed) probe.recovered = true;
    probe.recovery.cancel();
    operation.wait_destroyed();
    peer.wait_destroyed();
    const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    operation.await_resume();
    peer.await_resume();
    REQUIRE(drained);
    CHECK(failed);
    CHECK(probe.peer_received.load(std::memory_order_acquire));
    CHECK(completed);
    CHECK_FALSE(probe.recovered);
    REQUIRE(probe.failure);
    try { std::rethrow_exception(probe.failure); }
    catch (const std::runtime_error& error) { CHECK(std::string_view(error.what()) == "setup timer failed"); }
}

TEST_CASE("Connector observes unconstructible or rejected timers before TLS setup",
          "[http][setup][watchdog][setup-startup][issue-1287]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto workers = GENERATE(size_t{1}, size_t{2});
    const auto secure = GENERATE(false, true);
    const auto during_drain = GENERATE(false, true);
    CAPTURE(static_cast<int>(selected), workers, secure, during_drain);
#if ELIO_HAS_IO_URING
    if (selected == backend::io_uring && !elio::io::io_uring_backend::is_available())
        SKIP("io_uring unavailable on this host");
#else
    if (selected == backend::io_uring) SKIP("io_uring support is not compiled");
#endif
    backend_guard backend_scope(selected);
    auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    elio::tls::tls_context context(elio::tls::tls_mode::client);
    setup_probe probe;
    timer_guard timer(probe);
    startup_guard startup(during_drain);
    elio::runtime::scheduler scheduler(workers);
    scheduler.start();
    auto peer = scheduler.go_joinable(withhold_tls_reply(*listener, probe));
    auto operation = scheduler.go_joinable(exercise_setup(
        listener->local_address().port(), context, probe, secure));
    bool entered = true;
    bool closed = true;
    bool drained = false;
    std::thread shutdown;
    if (during_drain) {
        entered = observe([&] { return probe.before_start.load(std::memory_order_acquire); });
        shutdown = std::thread([&] { drained = scheduler.shutdown(elio::test::scaled_ms(5000)); });
        closed = observe([] {
            return elio::runtime::detail::graceful_admission_closed_for_test.load(std::memory_order_acquire);
        });
        probe.release_start.set();
    }
    const bool completed = observe([&] { return probe.done.load(std::memory_order_acquire); });
    if (!completed) probe.recovered = true;
    probe.recovery.cancel();
    operation.wait_destroyed();
    peer.wait_destroyed();
    if (shutdown.joinable()) shutdown.join();
    else drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    operation.await_resume();
    peer.await_resume();
    REQUIRE(drained);
    CHECK(entered);
    CHECK(closed);
    CHECK(completed);
    CHECK_FALSE(probe.recovered);
    CHECK_FALSE(probe.tls_entered.load(std::memory_order_acquire));
    CHECK_FALSE(probe.connect_entered.load(std::memory_order_acquire));
    CHECK_FALSE(probe.peer_received.load(std::memory_order_acquire));
    CHECK(probe.construction_failed.load(std::memory_order_acquire) == !during_drain);
    REQUIRE(probe.failure);
    if (during_drain) CHECK_THROWS_AS(std::rethrow_exception(probe.failure), std::logic_error);
    else CHECK_THROWS_AS(std::rethrow_exception(probe.failure), std::bad_alloc);
    // Construction injection is a pre-frame surrogate, not native exhaustion.
}

TEST_CASE("Connector joins an already-ready admitted watchdog before rethrowing",
          "[http][setup][watchdog][setup-ready-destruction][issue-1287]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto secure = GENERATE(false, true);
    const auto controller_failure = GENERATE(false, true);
    CAPTURE(static_cast<int>(selected), secure, controller_failure);
#if ELIO_HAS_IO_URING
    if (selected == backend::io_uring && !elio::io::io_uring_backend::is_available())
        SKIP("io_uring unavailable on this host");
#else
    if (selected == backend::io_uring) SKIP("io_uring support is not compiled");
#endif
    backend_guard backend_scope(selected);
    elio::tls::tls_context context(elio::tls::tls_mode::client);
    setup_probe probe;
    timer_guard timer(probe);
    elio::http::detail::setup_watchdog_wait_for_test.store(fail_ready_timer);
    // One worker holds the actual detached-frame test barrier while the other
    // observes the connector. No scheduling delay is used to create the race.
    elio::runtime::scheduler scheduler(2);
    ready_guard ready(probe);
    scheduler.start();
    auto operation = scheduler.go_joinable_to(0, exercise_setup(9, context, probe, secure));
    bool timer_ready = false;
    bool connector_observed = false;
    bool completed_before_destroy = false;
    bool destruction_observed = false;
    std::exception_ptr control_failure;
    try {
        timer_ready = observe([&] {
            return probe.timer_failed.load(std::memory_order_acquire) &&
                   probe.before_start.load(std::memory_order_acquire) &&
                   elio::coro::detail::detached_frame_destroy_paused_for_test.load(std::memory_order_acquire);
        });
        if (controller_failure) throw std::runtime_error("ready fixture controller failed");
        probe.ready_start.set();
        connector_observed = observe([&] {
            return probe.done.load(std::memory_order_acquire) ||
                   probe.destruction_observed.load(std::memory_order_acquire);
        });
        completed_before_destroy = probe.done.load(std::memory_order_acquire);
        destruction_observed = probe.destruction_observed.load(std::memory_order_acquire);
    } catch (...) { control_failure = std::current_exception(); }
    ready.release();
    if (control_failure) probe.recovery.cancel();
    const bool completed = observe([&] { return probe.done.load(std::memory_order_acquire); });
    probe.recovery.cancel();
    operation.wait_destroyed();
    const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    operation.await_resume();
    REQUIRE(drained);
    CHECK(timer_ready);
    if (controller_failure) {
        REQUIRE(control_failure);
        try { std::rethrow_exception(control_failure); }
        catch (const std::runtime_error& error) {
            CHECK(std::string_view(error.what()) == "ready fixture controller failed");
        }
    } else {
        if (control_failure) std::rethrow_exception(control_failure);
        CHECK(connector_observed);
        CHECK_FALSE(completed_before_destroy);
        CHECK(destruction_observed);
    }
    CHECK(completed);
    CHECK_FALSE(probe.connect_entered.load(std::memory_order_acquire));
    REQUIRE(probe.failure);
    try { std::rethrow_exception(probe.failure); }
    catch (const std::runtime_error& error) { CHECK(std::string_view(error.what()) == "ready setup timer failed"); }
}

TEST_CASE("Released watchdog fixture barrier cannot be armed by a late timer",
          "[http][setup][watchdog][setup-ready-late-arm][issue-1287]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    CAPTURE(static_cast<int>(selected));
#if ELIO_HAS_IO_URING
    if (selected == backend::io_uring && !elio::io::io_uring_backend::is_available())
        SKIP("io_uring unavailable on this host");
#else
    if (selected == backend::io_uring) SKIP("io_uring support is not compiled");
#endif
    backend_guard backend_scope(selected);
    setup_probe probe;
    probe.delay_arm = true;
    timer_guard timer(probe);
    elio::runtime::scheduler scheduler(2);
    ready_guard ready(probe);
    scheduler.start();
    auto delayed = scheduler.go_joinable_to(1, fail_ready_timer(
        std::chrono::steady_clock::now(), probe.recovery.get_token()));
    const bool parked = observe([&] { return probe.before_arm.load(std::memory_order_acquire); });
    ready.release();
    probe.recovery.cancel();
    delayed.wait_destroyed();
    const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    std::exception_ptr timer_failure;
    try { delayed.await_resume(); }
    catch (...) { timer_failure = std::current_exception(); }
    REQUIRE(drained);
    CHECK(parked);
    CHECK(probe.timer_failed.load(std::memory_order_acquire));
    CHECK_FALSE(elio::coro::detail::pause_before_detached_frame_destroy_for_test.load());
    CHECK_FALSE(elio::coro::detail::detached_frame_destroy_paused_for_test.load());
    REQUIRE(timer_failure);
    try { std::rethrow_exception(timer_failure); }
    catch (const std::runtime_error& error) { CHECK(std::string_view(error.what()) == "ready setup timer failed"); }
}
