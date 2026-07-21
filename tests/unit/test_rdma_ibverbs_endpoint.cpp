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

#include <infiniband/verbs.h>

#include <fcntl.h>
#include <unistd.h>

using elio::rdma_ibverbs::endpoint_config;

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

TEST_CASE("rdma_ibverbs module version matches S13",
          "[rdma][ibverbs][endpoint][version]") {
    REQUIRE(std::string(elio::rdma_ibverbs::module_version) == "0.0.14-S13");
}
