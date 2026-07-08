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

#include <elio/rdma_cm/rdma_cm.hpp>

#include <cerrno>
#include <utility>

using elio::rdma_cm::cm_id;
using elio::rdma_cm::cm_status;
using elio::rdma_cm::connect_options;

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

TEST_CASE("rdma_cm module version is the S8 string",
          "[rdma_cm][version]") {
    REQUIRE(std::string(elio::rdma_cm::module_version) == "0.0.11-S8");
}
