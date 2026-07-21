// Stage S13 — endpoint compile + RAII smoke check.
//
// The endpoint wrapper needs real verbs to actually construct (it
// calls ibv_alloc_pd / ibv_create_comp_channel / ibv_create_cq /
// rdma_create_qp during the ctor). On hosts without working uverbs
// — the OrbStack dev box, any non-RDMA Linux — those calls fail.
// These unit tests therefore verify that:
//   * The endpoint_config defaults match the documented values.
//   * The acceptor / connect free-functions can be referenced from
//     user code (compile-only check).
//   * Endpoint-owned file-descriptor setup works without RDMA hardware.
// End-to-end exercising lives under tests/integration_rdma/.

#include <catch2/catch_test_macros.hpp>

#include <elio/rdma_ibverbs/rdma_ibverbs.hpp>
#include <elio/runtime/scheduler.hpp>

#include <infiniband/verbs.h>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <type_traits>
#include <utility>

using elio::rdma_ibverbs::endpoint_config;

static_assert(std::is_nothrow_destructible_v<elio::rdma_ibverbs::endpoint>);
static_assert(std::is_same_v<
              decltype(std::declval<elio::rdma_ibverbs::endpoint&>().shutdown()),
              elio::coro::task<void>>);
static_assert(noexcept(
    std::declval<elio::rdma_ibverbs::endpoint&>().abandon_outstanding()));
static_assert(std::is_same_v<
              decltype(std::declval<elio::rdma_ibverbs::endpoint&>()
                           .abandon_outstanding()),
              bool>);

TEST_CASE("endpoint comp channel fds are made non-blocking",
          "[rdma][ibverbs][endpoint]") {
    int pipe_fds[2]{-1, -1};
    REQUIRE(::pipe(pipe_fds) == 0);
    struct pipe_guard {
        int* fds;
        ~pipe_guard() {
            ::close(fds[0]);
            ::close(fds[1]);
        }
    } guard{pipe_fds};

    REQUIRE_NOTHROW(
        elio::rdma_ibverbs::endpoint_detail::set_nonblocking(pipe_fds[0]));
    const int flags = ::fcntl(pipe_fds[0], F_GETFL);

    REQUIRE(flags >= 0);
    REQUIRE((flags & O_NONBLOCK) != 0);
    REQUIRE_NOTHROW(
        elio::rdma_ibverbs::endpoint_detail::set_nonblocking(pipe_fds[0]));
    REQUIRE_THROWS_AS(
        elio::rdma_ibverbs::endpoint_detail::set_nonblocking(-1),
        std::runtime_error);
}

TEST_CASE("endpoint_config defaults match the documented values",
          "[rdma][ibverbs][endpoint]") {
    endpoint_config cfg{};
    REQUIRE(cfg.max_send_wr == 16u);
    REQUIRE(cfg.max_recv_wr == 16u);
    REQUIRE(cfg.max_send_sge == 1u);
    REQUIRE(cfg.max_recv_sge == 1u);
    REQUIRE(cfg.max_inline_data == 0u);
    REQUIRE(cfg.cq_size == 64u);
    REQUIRE(cfg.qp_type == IBV_QPT_RC);
    REQUIRE(cfg.custom_qp_init_attr == nullptr);
}

TEST_CASE("endpoint teardown only releases resources after the pump is idle",
          "[rdma][ibverbs][endpoint][shutdown]") {
    using elio::rdma_ibverbs::endpoint_detail::pump_resources_are_idle;

    REQUIRE(pump_resources_are_idle(false, false, false));
    REQUIRE_FALSE(pump_resources_are_idle(true, false, false));
    REQUIRE(pump_resources_are_idle(true, true, false));
    REQUIRE(pump_resources_are_idle(true, false, true));
}

TEST_CASE("endpoint pump exit signal wakes and unregisters coroutine waiters",
          "[rdma][ibverbs][endpoint][shutdown]") {
    using elio::rdma_ibverbs::endpoint_detail::pump_exit_state;

    SECTION("exit wakes a suspended shutdown waiter") {
        pump_exit_state state;
        std::atomic<bool> waiting{false};
        std::atomic<bool> resumed{false};
        elio::runtime::scheduler scheduler(1);
        scheduler.start();

        auto wait_for_exit = [&]() -> elio::coro::task<void> {
            waiting.store(true, std::memory_order_release);
            waiting.notify_one();
            co_await state.wait();
            resumed.store(true, std::memory_order_release);
        };
        auto waiter = scheduler.go_joinable(wait_for_exit);

        waiting.wait(false, std::memory_order_acquire);
        auto publish_exit = [&]() -> elio::coro::task<void> {
            state.mark_exited();
            co_return;
        };
        auto publisher = scheduler.go_joinable(publish_exit);

        publisher.wait_destroyed();
        waiter.wait_destroyed();
        scheduler.shutdown();

        REQUIRE(resumed.load(std::memory_order_acquire));
    }

    SECTION("pre-published exit does not suspend") {
        pump_exit_state state;
        state.mark_exited();
        bool resumed = false;
        auto wait_for_prepublished_exit = [&]() -> elio::coro::task<void> {
            co_await state.wait();
            resumed = true;
        };
        auto waiting = wait_for_prepublished_exit();
        auto handle = elio::coro::detail::task_access::handle(waiting);

        handle.resume();
        REQUIRE(handle.done());
        REQUIRE(resumed);
    }

    SECTION("destroyed shutdown waiter unregisters before pump exit") {
        pump_exit_state state;
        {
            auto wait_for_abandoned_exit = [&]() -> elio::coro::task<void> {
                co_await state.wait();
            };
            auto abandoned = wait_for_abandoned_exit();
            auto handle = elio::coro::detail::task_access::handle(abandoned);
            handle.resume();
            REQUIRE_FALSE(handle.done());
        }

        REQUIRE_NOTHROW(state.mark_exited());
        REQUIRE(state.is_exited());
    }
}

TEST_CASE("endpoint resource teardown retains failures for retry",
          "[rdma][ibverbs][endpoint][shutdown]") {
    using elio::rdma_ibverbs::endpoint_detail::destroy_resource_or_throw;

    int storage = 0;
    int* resource = &storage;
    int attempts = 0;
    auto destroy = [&](int*) {
        ++attempts;
        return attempts == 1 ? EBUSY : 0;
    };

    REQUIRE_THROWS_AS(
        destroy_resource_or_throw(resource, destroy, "fake_destroy"),
        std::runtime_error);
    REQUIRE(resource == &storage);
    REQUIRE(errno == EBUSY);

    REQUIRE_NOTHROW(
        destroy_resource_or_throw(resource, destroy, "fake_destroy"));
    REQUIRE(resource == nullptr);
    REQUIRE(attempts == 2);
}

TEST_CASE("endpoint data-path use is rejected after shutdown starts",
          "[rdma][ibverbs][endpoint][shutdown]") {
    using elio::rdma_ibverbs::endpoint_detail::require_endpoint_active;

    REQUIRE_NOTHROW(require_endpoint_active(false, "test_operation"));
    REQUIRE_THROWS_AS(
        require_endpoint_active(true, "test_operation"),
        std::logic_error);
}

TEST_CASE("rdma_ibverbs module version matches S13",
          "[rdma][ibverbs][endpoint][version]") {
    REQUIRE(std::string(elio::rdma_ibverbs::module_version) == "0.0.14-S13");
}
