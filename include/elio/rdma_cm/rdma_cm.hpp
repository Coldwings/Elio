#pragma once

/// @file rdma_cm.hpp
/// @brief Umbrella header for the optional librdmacm-backed CM
///        adapter (stage S8).
///
/// To use: configure with `-DELIO_ENABLE_RDMA=ON
/// -DELIO_ENABLE_RDMA_CM=ON` and link against the `elio_rdma_cm`
/// CMake target. `ELIO_HAS_RDMA_CM` is defined in that case so user
/// code can guard CM-dependent paths.
///
/// The adapter does NOT couple Elio's RDMA data path to librdmacm —
/// the data path stays under `<elio/rdma/*>` and works with any
/// backend implementation. The CM helpers in this namespace are
/// purely a connection-bootstrap convenience: they hand the
/// application an established `ibv_qp*` (via the wrapped `cm_id`),
/// which the application then passes to
/// `elio::rdma::connection<Backend>` along with its dispatcher.
///
/// @code{.cpp}
/// elio::rdma_cm::event_channel ch;
/// auto id = co_await elio::rdma_cm::resolve(ch, dst_addr, sizeof(*dst_addr));
/// // ... rdma_create_qp(id.native(), pd, &qp_init_attr) ...
/// auto status = co_await elio::rdma_cm::complete_connect(ch, id);
/// if (!status.ok()) { ... }
///
/// elio::rdma::connection<my_backend> conn{id.qp(), disp};
/// co_await conn.send(buf);
/// @endcode

#include <elio/rdma_cm/cm_connect.hpp>
#include <elio/rdma_cm/cm_id.hpp>
#include <elio/rdma_cm/cm_listener.hpp>
#include <elio/rdma_cm/event_channel.hpp>

namespace elio::rdma_cm {

inline constexpr const char* module_version = "0.0.11-S8";

}  // namespace elio::rdma_cm
