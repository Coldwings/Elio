#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/http/client_base.hpp>
#include <elio/sync/event.hpp>
#include "../test_main.cpp"

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
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
    elio::coro::cancel_source recovery;
    std::atomic<bool> timer_failed{false};
    std::atomic<bool> peer_received{false};
    std::atomic<bool> done{false};
    bool recovered = false;
    std::exception_ptr failure;
};

std::atomic<setup_probe*> observed{nullptr};

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

task<void> exercise_setup(uint16_t port, elio::tls::tls_context& context, setup_probe& probe) {
    try {
        (void)co_await elio::http::client_connect_result("127.0.0.1", port, true, &context,
            elio::net::default_cached_resolve_options(), false, std::chrono::seconds(30),
            probe.recovery.get_token());
    } catch (...) { probe.failure = std::current_exception(); }
    probe.done.store(true, std::memory_order_release);
}
} // namespace

TEST_CASE("Connector timer failure cancels a pending real TLS handshake",
          "[http][setup][watchdog][setup-timer-failure]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    CAPTURE(static_cast<int>(selected));
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
    elio::runtime::scheduler scheduler(1);
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
