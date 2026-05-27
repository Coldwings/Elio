#pragma once

/// @file types.hpp
/// @brief Core value types for Elio's RDMA-Core abstraction (stage S0 stub).
///
/// At S0 these are placeholders to lock the API surface and namespace; the
/// real definitions land in S1 alongside op_state and the dispatcher.
///
/// Design goals (all implemented incrementally S1+):
///   * `buffer_view`     — { addr, len, lkey }, the thinnest WR-friendly
///                         description of a local buffer.
///   * `sge`             — single scatter-gather entry; backends accept
///                         `std::span<const sge>` once S5a lands.
///   * `remote_buffer`   — { addr, length, rkey } for RDMA WRITE/READ.
///   * `wc_status`       — enum normalising ibverbs IBV_WC_* status codes.
///   * `wc_result`       — { status, byte_len, imm_data, wc_flags } returned
///                         by every awaiter.
///   * `wr_id`           — uint64_t tagged pointer to op_state (S1).
///   * `send_flags`      — bitset { signaled, solicited, inline_send, fence }.
///
/// Everything below is a forward declaration only; using any of these
/// types from user code before S1 lands will fail to compile.

#include <cstddef>
#include <cstdint>

namespace elio::rdma {

// S1 will define these as concrete structs/enums. Keeping them as
// incomplete tags here lets dependent headers (backend_traits, etc.)
// reference the names without circular include hazards.

struct buffer_view;
struct sge;
struct remote_buffer;
enum class wc_status : std::uint32_t;
struct wc_result;
struct send_flags;

/// Opaque key the dispatcher hands to the backend's post_* calls; the
/// backend echoes it verbatim into the WR's `wr_id` field. Tag-bit
/// encoding (S1) hides an op_state* inside.
using wr_id = std::uint64_t;

}  // namespace elio::rdma
