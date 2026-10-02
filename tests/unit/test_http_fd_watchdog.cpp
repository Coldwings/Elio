#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <elio/http/client_base.hpp>
#include <elio/sync/event.hpp>
#include "../test_main.cpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <unistd.h>

namespace {
using elio::coro::task;
using elio::coro::cancel_result;

template<typename Predicate>
bool wait_for(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + elio::test::scaled_ms(5000);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

struct socket_pair {
    std::array<int, 2> descriptors{-1, -1};
    socket_pair() {
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                             0, descriptors.data()) == 0);
    }
    ~socket_pair() {
        for (int fd : descriptors) if (fd >= 0) ::close(fd);
    }
    socket_pair(const socket_pair&) = delete;
    socket_pair& operator=(const socket_pair&) = delete;
};

struct watchdog_control {
    elio::sync::event entered;
    elio::sync::event release;
    elio::sync::event settled;
    std::atomic<bool> finished{false};
    std::atomic<bool> cancelled{false};
    bool throw_after_cancel = false;
    bool hold_after_cancel = false;
};

std::atomic<watchdog_control*> active_control{nullptr};

task<cancel_result> controlled_wait(std::chrono::nanoseconds,
                                  elio::coro::cancel_token token) {
    auto* control = active_control.load(std::memory_order_acquire);
    if (!control) throw std::logic_error("missing watchdog control");
    control->entered.set();
    auto result = co_await control->release.wait(std::move(token));
    control->cancelled.store(result == cancel_result::cancelled, std::memory_order_release);
    if (result == cancel_result::cancelled && control->hold_after_cancel) {
        co_await control->settled.wait();
    }
    control->finished.store(true, std::memory_order_release);
    if (result == cancel_result::cancelled && control->throw_after_cancel) {
        throw std::logic_error("secondary watchdog failure");
    }
    co_return result;
}

struct watchdog_hook_guard {
    explicit watchdog_hook_guard(watchdog_control& control) {
        active_control.store(&control, std::memory_order_release);
        elio::http::detail::fd_watchdog_shutdowns_for_test.store(0);
        elio::http::detail::fd_watchdog_wait_for_test.store(controlled_wait,
                                                        std::memory_order_release);
    }
    ~watchdog_hook_guard() {
        elio::http::detail::fd_watchdog_wait_for_test.store(nullptr, std::memory_order_release);
        active_control.store(nullptr, std::memory_order_release);
    }
};

// Declared after the scheduler so every exit releases the controlled timer
// before scheduler destruction, including a failed main-thread assertion.
struct release_guard {
    watchdog_control& control;
    ~release_guard() {
        control.release.set();
        control.settled.set();
    }
};

enum class exception_site { task_creation, suspended_operation };

task<elio::io::io_result> throw_after_watchdog_entry(watchdog_control& control) {
    co_await control.entered.wait();
    throw std::runtime_error("original operation failure");
    co_return elio::io::io_result{};
}

task<elio::io::io_result> complete_after_watchdog_entry(
        watchdog_control& control, elio::io::io_result result) {
    co_await control.entered.wait();
    co_return result;
}

void require_socket_usable(int fd, int peer) {
    const char sent = 'x';
    REQUIRE(::send(fd, &sent, 1, MSG_NOSIGNAL) == 1);
    char received = 0;
    REQUIRE(::recv(peer, &received, 1, MSG_DONTWAIT) == 1);
    REQUIRE(received == sent);
}
} // namespace

TEST_CASE("FD watchdog is joined before propagating operation exceptions",
          "[http][watchdog][exception]") {
    const auto site = GENERATE(exception_site::task_creation,
                               exception_site::suspended_operation);
    const bool secondary_failure = GENERATE(false, true);
    socket_pair original;
    socket_pair replacement;
    watchdog_control control;
    control.throw_after_cancel = secondary_failure;
    watchdog_hook_guard hook(control);
    auto timed_out = std::make_shared<std::atomic<bool>>(false);
    std::exception_ptr failure;
    std::atomic<bool> done{false};
    bool finished_at_return = false;
    elio::runtime::scheduler scheduler(2);
    release_guard release{control};
    scheduler.start();
    scheduler.go([&]() -> task<void> {
        try {
            (void)co_await elio::http::detail::await_fd_operation_with_watchdog(
                [&]() -> task<elio::io::io_result> {
                    // A non-coroutine factory models allocation/setup failure
                    // before an operation task can be returned to the awaiter.
                    if (site == exception_site::task_creation) throw std::bad_alloc();
                    return throw_after_watchdog_entry(control);
                }, &scheduler, original.descriptors[0], std::chrono::hours(1), timed_out);
        } catch (...) {
            failure = std::current_exception();
        }
        finished_at_return = control.finished.load(std::memory_order_acquire);
        done.store(true, std::memory_order_release);
    });
    const bool completed = wait_for([&] { return done.load(std::memory_order_acquire); });
    int reused = -1;
    if (completed) {
        // Replace only the descriptor owned by this fixture, after the request
        // has returned. A leaked watchdog would now target this new socket.
        reused = ::dup2(replacement.descriptors[0], original.descriptors[0]);
    }
    control.release.set();
    const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(completed);
    REQUIRE(reused == original.descriptors[0]);
    CHECK(finished_at_return);
    REQUIRE(control.finished.load(std::memory_order_acquire));
    CHECK(control.cancelled.load(std::memory_order_acquire));
    CHECK_FALSE(timed_out->load(std::memory_order_acquire));
    CHECK(elio::http::detail::fd_watchdog_shutdowns_for_test.load() == 0);
    REQUIRE(failure);
    if (site == exception_site::task_creation) {
        REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::bad_alloc);
    } else {
        REQUIRE_THROWS_WITH(std::rethrow_exception(failure), "original operation failure");
    }
    require_socket_usable(original.descriptors[0], replacement.descriptors[1]);
}

TEST_CASE("FD watchdog cleanup preserves successful and failed I/O results",
          "[http][watchdog]") {
    const int32_t expected = GENERATE(7, -EPIPE);
    socket_pair sockets;
    watchdog_control control;
    watchdog_hook_guard hook(control);
    auto timed_out = std::make_shared<std::atomic<bool>>(false);
    elio::io::io_result result{};
    std::exception_ptr failure;
    std::atomic<bool> done{false};
    bool finished_at_return = false;
    elio::runtime::scheduler scheduler(2);
    release_guard release{control};
    scheduler.start();
    scheduler.go([&]() -> task<void> {
        try {
            result = co_await elio::http::detail::await_fd_operation_with_watchdog(
                [&] { return complete_after_watchdog_entry(control, {expected, 0x1234}); },
                &scheduler, sockets.descriptors[0], std::chrono::hours(1), timed_out);
        } catch (...) {
            failure = std::current_exception();
        }
        finished_at_return = control.finished.load(std::memory_order_acquire);
        done.store(true, std::memory_order_release);
    });
    const bool completed = wait_for([&] { return done.load(std::memory_order_acquire); });
    control.release.set();
    const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(completed);
    REQUIRE_FALSE(failure);
    REQUIRE(finished_at_return);
    REQUIRE(result.result == expected);
    REQUIRE(result.flags == 0x1234);
    REQUIRE(control.cancelled.load(std::memory_order_acquire));
    REQUIRE_FALSE(timed_out->load(std::memory_order_acquire));
    REQUIRE(elio::http::detail::fd_watchdog_shutdowns_for_test.load() == 0);
    require_socket_usable(sockets.descriptors[0], sockets.descriptors[1]);
}

TEST_CASE("Rejected FD watchdog admission does not invoke sibling I/O",
          "[http][watchdog][exception][issue-1282]") {
    socket_pair sockets;
    auto timed_out = std::make_shared<std::atomic<bool>>(false);
    bool invoked = false;
    elio::runtime::scheduler rejecting_scheduler(1);
    // An unstarted scheduler has the same initial-admission rejection as
    // graceful drain. The factory is observable and needs no forced recovery.
    auto guarded = elio::http::detail::await_fd_operation_with_watchdog(
        [&] {
            invoked = true;
            return []() -> task<elio::io::io_result> {
                co_return elio::io::io_result{1, 0};
            }();
        }, &rejecting_scheduler, sockets.descriptors[0], std::chrono::hours(1), timed_out);
    auto handle = elio::coro::detail::task_access::handle(guarded);
    handle.resume();
    REQUIRE(handle.done());
    CHECK_FALSE(invoked);
    REQUIRE_THROWS_WITH(guarded.await_resume(),
                        "scheduler rejected joinable task before execution");
    CHECK_FALSE(timed_out->load(std::memory_order_acquire));
    require_socket_usable(sockets.descriptors[0], sockets.descriptors[1]);
}

TEST_CASE("FD watchdog expiry still interrupts stalled I/O",
          "[http][watchdog][timeout]") {
    socket_pair sockets;
    // Give the Elio stream its own descriptor; the fixture retains the alias
    // for main-thread checks only after I/O and scheduler drain have finished.
    const int stream_fd = ::dup(sockets.descriptors[0]);
    REQUIRE(stream_fd >= 0);
    elio::net::tcp_stream stream(stream_fd);
    watchdog_control control;
    watchdog_hook_guard hook(control);
    auto timed_out = std::make_shared<std::atomic<bool>>(false);
    elio::io::io_result result{-EIO, 0};
    std::exception_ptr failure;
    elio::coro::cancel_source cleanup;
    std::atomic<bool> read_staged{false};
    std::atomic<bool> done{false};
    char buffer = 0;
    elio::runtime::scheduler scheduler(2);
    release_guard release{control};
    scheduler.start();
    scheduler.go([&]() -> task<void> {
        try {
            result = co_await elio::http::detail::await_fd_operation_with_watchdog(
                [&]() -> task<elio::io::io_result> {
                    co_await control.entered.wait();
                    elio::io::detail::arm_next_cancellable_recv_staged_for_test(read_staged);
                    co_return co_await stream.read(&buffer, 1, cleanup.get_token());
                }, &scheduler, stream.fd(), std::chrono::hours(1), timed_out);
        } catch (...) {
            failure = std::current_exception();
        }
        done.store(true, std::memory_order_release);
    });
    const bool staged = wait_for([&] { return read_staged.load(std::memory_order_acquire); });
    control.release.set();
    const bool completed = wait_for([&] { return done.load(std::memory_order_acquire); });
    cleanup.cancel();
    const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(staged);
    REQUIRE(completed);
    REQUIRE_FALSE(failure);
    REQUIRE(result.result == 0);
    REQUIRE(control.finished.load(std::memory_order_acquire));
    REQUIRE_FALSE(control.cancelled.load(std::memory_order_acquire));
    REQUIRE(timed_out->load(std::memory_order_acquire));
    REQUIRE(elio::http::detail::fd_watchdog_shutdowns_for_test.load() == 1);
    const char sent = 'x';
    REQUIRE(::send(sockets.descriptors[0], &sent, 1, MSG_NOSIGNAL) == -1);
    REQUIRE(errno == EPIPE);
}

TEST_CASE("FD watchdog exception propagation waits beyond cancellation delivery",
          "[http][watchdog][exception][join]") {
    socket_pair sockets;
    watchdog_control control;
    control.hold_after_cancel = true;
    watchdog_hook_guard hook(control);
    auto timed_out = std::make_shared<std::atomic<bool>>(false);
    std::exception_ptr failure;
    std::atomic<bool> done{false};
    // With one worker, a cancel-only helper would finish its caller before
    // the watchdog publishes cancellation. A real join instead yields to it.
    elio::runtime::scheduler scheduler(1);
    release_guard release{control};
    scheduler.start();
    scheduler.go([&]() -> task<void> {
        try {
            (void)co_await elio::http::detail::await_fd_operation_with_watchdog(
                []() -> task<elio::io::io_result> { throw std::bad_alloc(); },
                &scheduler, sockets.descriptors[0], std::chrono::hours(1), timed_out);
        } catch (...) {
            failure = std::current_exception();
        }
        done.store(true, std::memory_order_release);
    });
    const bool cancellation_delivered = wait_for([&] {
        return control.cancelled.load(std::memory_order_acquire);
    });
    const bool returned_before_settlement = done.load(std::memory_order_acquire);
    const bool finished_before_settlement = control.finished.load(std::memory_order_acquire);
    control.release.set();
    control.settled.set();
    const bool completed = wait_for([&] { return done.load(std::memory_order_acquire); });
    const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(cancellation_delivered);
    REQUIRE_FALSE(returned_before_settlement);
    REQUIRE_FALSE(finished_before_settlement);
    REQUIRE(completed);
    REQUIRE(control.finished.load(std::memory_order_acquire));
    REQUIRE(failure);
    REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::bad_alloc);
    REQUIRE_FALSE(timed_out->load(std::memory_order_acquire));
    REQUIRE(elio::http::detail::fd_watchdog_shutdowns_for_test.load() == 0);
    require_socket_usable(sockets.descriptors[0], sockets.descriptors[1]);
}
