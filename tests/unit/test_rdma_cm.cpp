// Stage S8 — elio_rdma_cm headers.
//
// This test is a compile-and-RAII smoke check. The OrbStack
// development host runs the librdmacm headers fine but the
// userspace verbs ABI is broken (open of /dev/infiniband/uverbs0
// returns EPERM), so calls into rdma_create_event_channel and
// friends are unreliable at runtime. End-to-end CM/data-path
// validation happens on a separate host with a working rxe stack
// (planned as part of S10 integration tests).
//
// What we verify here:
//   * The cm_id RAII wrapper safely handles the null case: default
//     construction, move-construct, move-assign with no librdmacm
//     calls when the pointer is null.
//   * The connect_options aggregate has the documented defaults.
//   * The umbrella header pulls in every public symbol.

#include <catch2/catch_test_macros.hpp>

#include <elio/io/io_awaitables.hpp>
#include <elio/rdma_cm/rdma_cm.hpp>
#include <elio/runtime/scheduler.hpp>

#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "../test_main.cpp"

using elio::rdma_cm::cm_id;
using elio::rdma_cm::cm_status;
using elio::rdma_cm::connect_options;

namespace {

bool wait_for_flag(const std::atomic<bool>& flag) {
    const auto deadline =
        std::chrono::steady_clock::now() + elio::test::scaled_ms(2000);
    while (!flag.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag.load(std::memory_order_acquire);
}

}  // namespace

TEST_CASE("cm_id default construct and move are safe with no librdmacm call",
          "[rdma_cm][cm_id]") {
    cm_id a{};
    REQUIRE_FALSE(static_cast<bool>(a));
    REQUIRE(a.native() == nullptr);
    REQUIRE(a.qp() == nullptr);
    REQUIRE(a.pd() == nullptr);
    REQUIRE(a.verbs() == nullptr);

    cm_id b{std::move(a)};
    REQUIRE_FALSE(static_cast<bool>(a));
    REQUIRE_FALSE(static_cast<bool>(b));

    cm_id c{};
    c = std::move(b);
    REQUIRE_FALSE(static_cast<bool>(c));

    // release on a null cm_id returns nullptr without crashing.
    REQUIRE(c.release() == nullptr);
}

TEST_CASE("connect_options defaults match documented values",
          "[rdma_cm][connect]") {
    connect_options opts{};
    REQUIRE(opts.src == nullptr);
    REQUIRE(opts.timeout_ms == 2000);
    REQUIRE(opts.port_space == RDMA_PS_TCP);
}

TEST_CASE("cm_status::ok matches status==0",
          "[rdma_cm][status]") {
    cm_status zero{0};
    REQUIRE(zero.ok());
    cm_status err{-22};
    REQUIRE_FALSE(err.ok());
}

TEST_CASE("cm_status helpers preserve the documented -errno contract",
          "[rdma_cm][status]") {
    auto zero = elio::rdma_cm::detail::make_cm_status(0);
    REQUIRE(zero.status == 0);
    REQUIRE(zero.ok());

    auto positive_errno = elio::rdma_cm::detail::make_cm_status(EAGAIN);
    REQUIRE(positive_errno.status == -EAGAIN);
    REQUIRE_FALSE(positive_errno.ok());

    auto negative_errno =
        elio::rdma_cm::detail::make_cm_status(-ETIMEDOUT);
    REQUIRE(negative_errno.status == -ETIMEDOUT);
    REQUIRE_FALSE(negative_errno.ok());
}

TEST_CASE("cm_status reports invalid cm_id as exact -EINVAL",
          "[rdma_cm][status][cm_id]") {
    cm_id id{};
    auto status = elio::rdma_cm::detail::cm_id_status(id);
    REQUIRE(status.status == -EINVAL);
    REQUIRE_FALSE(status.ok());
}

TEST_CASE("event backlog preserves events for their matching cm_id",
          "[rdma_cm][event_channel][routing]") {
    auto* id_a = reinterpret_cast<rdma_cm_id*>(0x1000);
    auto* id_b = reinterpret_cast<rdma_cm_id*>(0x2000);

    rdma_cm_event event_b{};
    event_b.id = id_b;
    event_b.event = RDMA_CM_EVENT_ROUTE_RESOLVED;

    rdma_cm_event event_a{};
    event_a.id = id_a;
    event_a.event = RDMA_CM_EVENT_ADDR_RESOLVED;

    elio::rdma_cm::detail::event_backlog backlog;
    backlog.stash(&event_b);
    backlog.stash(&event_a);

    auto* got_a = backlog.take_if([id_a](rdma_cm_event* event) {
        return event && event->id == id_a;
    });
    REQUIRE(got_a == &event_a);
    REQUIRE(backlog.size() == 1);

    auto* got_b = backlog.take_if([id_b](rdma_cm_event* event) {
        return event && event->id == id_b;
    });
    REQUIRE(got_b == &event_b);
    REQUIRE(backlog.empty());
}

TEST_CASE("backlog waiter registry cancels fd poll waiters on stash wake",
          "[rdma_cm][event_channel][routing][regression]") {
    elio::rdma_cm::detail::backlog_wait_registry registry;

    auto source = std::make_shared<elio::coro::cancel_source>();
    auto token = source->get_token();
    auto subscription = registry.subscribe(source);

    REQUIRE_FALSE(token.is_cancelled());
    registry.cancel_all();
    REQUIRE(token.is_cancelled());

    subscription.reset();
}

TEST_CASE("backlog waiter registry wakes a coroutine blocked in fd poll",
          "[rdma_cm][event_channel][routing][regression]") {
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    REQUIRE(fd >= 0);

    elio::rdma_cm::detail::backlog_wait_registry registry;
    auto source = std::make_shared<elio::coro::cancel_source>();
    auto subscription = registry.subscribe(source);

    std::atomic<bool> started{false};
    std::atomic<bool> cancelled{false};

    auto poller = [&]() -> elio::coro::task<void> {
        started.store(true, std::memory_order_release);
        auto result = co_await elio::io::async_poll_read(
            fd, source->get_token());
        cancelled.store(result.was_cancelled(), std::memory_order_release);
    };

    elio::runtime::scheduler sched(1);
    sched.start();
    sched.go(poller);

    REQUIRE(wait_for_flag(started));
    registry.cancel_all();

    REQUIRE(sched.shutdown(elio::test::scaled_ms(2000)));
    subscription.reset();
    ::close(fd);

    REQUIRE(cancelled.load(std::memory_order_acquire));
}

TEST_CASE("backlog waiter subscription unregisters completed waiters",
          "[rdma_cm][event_channel][routing][regression]") {
    elio::rdma_cm::detail::backlog_wait_registry registry;

    auto source = std::make_shared<elio::coro::cancel_source>();
    auto token = source->get_token();
    {
        auto subscription = registry.subscribe(source);
        REQUIRE_FALSE(token.is_cancelled());
    }

    registry.cancel_all();
    REQUIRE_FALSE(token.is_cancelled());
}

TEST_CASE("rdma_cm module version is the S8 string",
          "[rdma_cm][version]") {
    REQUIRE(std::string(elio::rdma_cm::module_version) == "0.0.11-S8");
}
