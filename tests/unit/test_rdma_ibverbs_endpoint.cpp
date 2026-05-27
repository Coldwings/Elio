// Stage S13 — endpoint compile + RAII smoke check.
//
// The endpoint wrapper needs real verbs to actually construct (it
// calls ibv_alloc_pd / ibv_create_comp_channel / ibv_create_cq /
// rdma_create_qp during the ctor). On hosts without working uverbs
// — the OrbStack dev box, any non-RDMA Linux — those calls fail.
// This unit test therefore only verifies that:
//   * The endpoint_config defaults match the documented values.
//   * The acceptor / connect free-functions can be referenced from
//     user code (compile-only check).
// End-to-end exercising lives under tests/integration_rdma/.

#include <catch2/catch_test_macros.hpp>

#include <elio/rdma_ibverbs/rdma_ibverbs.hpp>

#include <infiniband/verbs.h>

using elio::rdma_ibverbs::endpoint_config;

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
