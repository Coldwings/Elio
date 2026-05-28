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
/// Through S8 the umbrella re-exports: types, op_state, dispatcher,
/// backend_traits (incl. the extended `backend_with_mr` requiring
/// `lkey_of` / `rkey_of`), polymorphic_backend, connection, all four
/// data-path awaiters (SEND / RECV / RDMA_WRITE / RDMA_READ), the
/// multi-SGE / inline-send extensions, the shared receive queue
/// abstraction (`srq.hpp`), the `memory_region<Backend>` RAII
/// wrapper (`memory_region.hpp`), and the default `cq_pump`
/// coroutine that binds a completion-channel fd to an `io_context`
/// (`cq_pump.hpp`). The optional librdmacm-backed CM helper lives
/// in a separate `elio_rdma_cm` CMake target (`<elio/rdma_cm/*>`).
///
/// See spec/rdma-support-plan.md (feature/rdma-support local branch) for
/// the full design plan and stage breakdown.

#include <elio/rdma/types.hpp>
#include <elio/rdma/op_state.hpp>
#include <elio/rdma/completion.hpp>
#include <elio/rdma/backend_traits.hpp>
#include <elio/rdma/operations.hpp>
#include <elio/rdma/connection.hpp>
#include <elio/rdma/memory_region.hpp>
#include <elio/rdma/srq.hpp>
#include <elio/rdma/cq_pump.hpp>

namespace elio::rdma {

/// Module version string, bumped per stage so downstream code can
/// feature-detect during this incremental rollout.
inline constexpr const char* module_version = "0.0.15-S15";

}  // namespace elio::rdma
