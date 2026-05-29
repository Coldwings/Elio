#pragma once

/// @file rdma_ibverbs.hpp
/// @brief Umbrella header for the optional libibverbs adapter.
///
/// Enable via `-DELIO_ENABLE_RDMA=ON -DELIO_ENABLE_RDMA_IBVERBS=ON`
/// and link against the `elio_rdma_ibverbs` CMake target. The
/// preprocessor macro `ELIO_HAS_RDMA_IBVERBS` is defined when the
/// module is available.
///
/// Re-exports:
///   * `ibverbs_backend.hpp` — public static-traits backend that
///     forwards every Elio data-path call to the matching libibverbs
///     symbol. Save the typedef `using my_backend =
///     elio::rdma_ibverbs::ibverbs_backend` and the rest of Elio's
///     surface (connection, memory_region, srq) works against real
///     verbs out of the box.
///   * `cq_drain.hpp` — `make_cq_drain(ibv_cq*, ibv_comp_channel*)`
///     returns the drain callable that should be passed straight
///     into `elio::rdma::cq_pump`. Implements the canonical
///     ibv_get_cq_event → ack → re-arm → poll loop with the
///     status-translation helper plumbed through.
///   * `endpoint.hpp` — high-level wrapper that bundles PD + CQ +
///     comp_channel + QP + dispatcher + cq_pump into a single
///     RAII object so a user can stand up an RDMA endpoint without
///     touching libibverbs directly.

#include <elio/rdma_ibverbs/cq_drain.hpp>
#include <elio/rdma_ibverbs/endpoint.hpp>
#include <elio/rdma_ibverbs/ibverbs_backend.hpp>

namespace elio::rdma_ibverbs {

inline constexpr const char* module_version = "0.0.14-S13";

}  // namespace elio::rdma_ibverbs
