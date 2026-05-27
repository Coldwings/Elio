#pragma once

/// @file rdma.hpp
/// @brief Umbrella header for Elio's optional RDMA-Core abstraction layer.
///
/// Elio provides a thin, protocol-agnostic abstraction over RDMA-Core verbs.
/// The abstraction itself is header-only and does NOT depend on libibverbs
/// or librdmacm — consumers plug in their own backend (via template traits
/// or a polymorphic_backend pure-virtual class) and link those libraries
/// themselves. This keeps Elio's core lightweight and lets downstream code
/// pick between real ibverbs, an in-memory mock (for tests), or any other
/// implementation that satisfies elio::rdma::backend_traits.
///
/// To enable this module, configure with `-DELIO_ENABLE_RDMA=ON`. The
/// `ELIO_HAS_RDMA` macro is defined in that case so user code can guard
/// optional RDMA paths:
///
/// @code{.cpp}
/// #if ELIO_HAS_RDMA
/// #include <elio/rdma/rdma.hpp>
/// // ...
/// #endif
/// @endcode
///
/// ## Stage status
///
/// This umbrella currently re-exports only the type and traits stubs from
/// stage S0 (skeleton). Subsequent stages will add:
///   * S1: op_state (UAF-safe completion routing) + dispatcher
///   * S2: backend_traits concept + polymorphic_backend
///   * S3-S4: send/recv/rdma_write/rdma_read awaitables
///   * S5: SGE, inline send, SRQ
///   * S6: memory_region<Backend> RAII
///   * S7: cq_pump coroutine (io_context binding)
///
/// See spec/rdma-support-plan.md (feature/rdma-support local branch) for
/// the full design plan and stage breakdown.

#include <elio/rdma/types.hpp>
#include <elio/rdma/op_state.hpp>
#include <elio/rdma/completion.hpp>
#include <elio/rdma/backend_traits.hpp>
#include <elio/rdma/connection.hpp>

namespace elio::rdma {

/// Module version string, bumped per stage so downstream code can
/// feature-detect during this incremental rollout.
inline constexpr const char* module_version = "0.0.3-S2";

}  // namespace elio::rdma
