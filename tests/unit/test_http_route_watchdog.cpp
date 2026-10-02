#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/http/detail/route_operation.hpp>
#include <elio/net/tcp.hpp>
#include "../test_main.cpp"

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <new>
#include <thread>
#include <unistd.h>

namespace {
using elio::coro::task;
using backend = elio::io::io_context::backend_type;

struct backend_guard {
    backend previous;
    explicit backend_guard(backend value)
        : previous(elio::runtime::detail::worker_io_backend_for_test.exchange(value)) {}
    ~backend_guard() { elio::runtime::detail::worker_io_backend_for_test.store(previous); }
};

struct socket_pair {
    std::array<int, 2> fds{-1, -1};
    socket_pair() {
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                             0, fds.data()) == 0);
    }
    ~socket_pair() { for (int fd : fds) if (fd >= 0) ::close(fd); }
    socket_pair(const socket_pair&) = delete;
    socket_pair& operator=(const socket_pair&) = delete;
};

struct route_probe {
    std::atomic<bool> entered{false};
    std::atomic<bool> invoked{false};
    std::atomic<bool> done{false};
    std::atomic<bool> recovered{false};
    std::exception_ptr failure;
    elio::coro::cancel_source recovery;
    std::array<char, 1> storage{};
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

task<void> exercise_route(elio::net::tcp_stream& stream, route_probe& probe,
                          bool during_drain) {
    probe.entered.store(true, std::memory_order_release);
    if (during_drain) {
        while (!elio::runtime::detail::graceful_admission_closed_for_test.load(
                   std::memory_order_acquire))
            co_await elio::coro::yield();
    }
    try {
        (void)co_await elio::http::detail::await_route_operation<elio::io::io_result>(
            [&](elio::coro::cancel_token token) -> task<elio::io::io_result> {
                // This is a non-coroutine factory: invocation is recorded even
                // before the returned read coroutine can begin execution.
                probe.invoked.store(true, std::memory_order_release);
                return stream.read(probe.storage.data(), probe.storage.size(), std::move(token));
            }, probe.recovery.get_token(),
            std::chrono::steady_clock::now() + std::chrono::seconds(30));
    } catch (...) {
        probe.failure = std::current_exception();
    }
    probe.done.store(true, std::memory_order_release);
}

task<void> recover_accepted_route(route_probe& probe) {
    while (!elio::runtime::detail::graceful_admission_closed_for_test.load(
               std::memory_order_acquire))
        co_await elio::coro::yield();
    const auto deadline = std::chrono::steady_clock::now() + elio::test::scaled_ms(2000);
    while (!probe.done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            probe.recovered.store(true, std::memory_order_release);
            probe.recovery.cancel();
            break;
        }
        co_await elio::coro::yield();
    }
}

void fail_construction() { throw std::bad_alloc(); }

struct construction_guard {
    construction_guard() {
        elio::http::detail::route_watchdog_before_construct_for_test.store(
            fail_construction, std::memory_order_release);
    }
    ~construction_guard() {
        elio::http::detail::route_watchdog_before_construct_for_test.store(
            nullptr, std::memory_order_release);
    }
};

struct drain_hook_guard {
    drain_hook_guard() {
        elio::runtime::detail::graceful_admission_closed_for_test.store(false);
    }
    ~drain_hook_guard() {
        elio::runtime::detail::graceful_admission_closed_for_test.store(false);
    }
};
} // namespace

TEST_CASE("Layered route watchdog construction precedes sibling I/O",
          "[http][routes][watchdog][issue-1249]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    CAPTURE(static_cast<int>(selected));
#if ELIO_HAS_IO_URING
    if (selected == backend::io_uring && !elio::io::io_uring_backend::is_available())
        SKIP("io_uring unavailable on this host");
#else
    if (selected == backend::io_uring) SKIP("io_uring support is not compiled");
#endif
    backend_guard backend_scope(selected);
    socket_pair sockets;
    const int fd = ::dup(sockets.fds[0]);
    REQUIRE(fd >= 0);
    elio::net::tcp_stream stream(fd);
    route_probe probe;
    construction_guard injection;
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto operation = scheduler.go_joinable(exercise_route(stream, probe, false));
    const bool completed = observe([&] { return probe.done.load(std::memory_order_acquire); });
    if (!completed) {
        probe.recovered.store(true, std::memory_order_release);
        probe.recovery.cancel();
    }
    const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    operation.wait_destroyed();
    operation.await_resume();
    REQUIRE(drained);
    CHECK(completed);
    CHECK_FALSE(probe.invoked.load(std::memory_order_acquire));
    CHECK_FALSE(probe.recovered.load(std::memory_order_acquire));
    REQUIRE(probe.failure);
    REQUIRE_THROWS_AS(std::rethrow_exception(probe.failure), std::bad_alloc);
    // The injection models pre-frame construction, not a native allocator.
}

TEST_CASE("Accepted layered routes observe rejected watchdogs during graceful drain",
          "[http][routes][watchdog][issue-1249]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    CAPTURE(static_cast<int>(selected));
#if ELIO_HAS_IO_URING
    if (selected == backend::io_uring && !elio::io::io_uring_backend::is_available())
        SKIP("io_uring unavailable on this host");
#else
    if (selected == backend::io_uring) SKIP("io_uring support is not compiled");
#endif
    backend_guard backend_scope(selected);
    socket_pair sockets;
    const int fd = ::dup(sockets.fds[0]);
    REQUIRE(fd >= 0);
    elio::net::tcp_stream stream(fd);
    route_probe probe;
    drain_hook_guard admission;
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto operation = scheduler.go_joinable(exercise_route(stream, probe, true));
    // Both roots are admitted before shutdown closes independent admission.
    // Recovery uses cooperative cancellation and normal joins, never force-stop.
    auto recovery = scheduler.go_joinable(recover_accepted_route(probe));
    const bool entered = observe([&] { return probe.entered.load(std::memory_order_acquire); });
    const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    operation.wait_destroyed();
    recovery.wait_destroyed();
    operation.await_resume();
    recovery.await_resume();
    REQUIRE(entered);
    REQUIRE(drained);
    CHECK(probe.done.load(std::memory_order_acquire));
    CHECK_FALSE(probe.invoked.load(std::memory_order_acquire));
    CHECK_FALSE(probe.recovered.load(std::memory_order_acquire));
    REQUIRE(probe.failure);
    REQUIRE_THROWS_WITH(std::rethrow_exception(probe.failure),
                        "scheduler rejected joinable task before execution");
}
