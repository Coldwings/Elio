#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/http/client_base.hpp>
#include <elio/sync/event.hpp>
#include "../test_main.cpp"

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
using elio::coro::task;
using backend = elio::io::io_context::backend_type;

struct backend_guard {
    backend previous;
    explicit backend_guard(backend value)
        : previous(elio::runtime::detail::worker_io_backend_for_test.exchange(value)) {}
    ~backend_guard() { elio::runtime::detail::worker_io_backend_for_test.store(previous); }
};

struct setup_probe {
    elio::sync::event hello_received;
    elio::sync::event release_start;
    elio::coro::cancel_source recovery;
    std::atomic<bool> timer_failed{false};
    std::atomic<bool> peer_received{false};
    std::atomic<bool> done{false};
    std::atomic<bool> before_start{false};
    std::atomic<bool> construction_failed{false};
    std::atomic<bool> tls_entered{false};
    std::atomic<bool> connect_entered{false};
    bool recovered = false;
    std::exception_ptr failure;
};

std::atomic<setup_probe*> observed{nullptr};

task<void> pause_before_timer() {
    auto& probe = *observed.load(std::memory_order_acquire);
    probe.before_start.store(true, std::memory_order_release);
    (void)co_await probe.release_start.wait(probe.recovery.get_token());
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
