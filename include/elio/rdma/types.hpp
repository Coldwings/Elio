#pragma once

/// @file types.hpp
/// @brief Core value types for Elio's RDMA-Core abstraction.
///
/// Stage S1: concrete definitions for the data-path value types. The
/// abstraction itself is header-only and does not include `<infiniband/verbs.h>`;
/// these structs are normalised, backend-agnostic mirrors that the user's
/// backend translates to/from real ibverbs structures.

#include <cassert>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <endian.h>

namespace elio::rdma {

/// Opaque key the dispatcher hands to the backend's `post_*` calls.
/// The backend echoes it verbatim into the WR's `wr_id` field. S1 encodes
/// a raw `detail::op_state*`; S5c (SRQ) will use the low bit to tag
/// SRQ-completion routing.
using wr_id = std::uint64_t;

/// Thinnest WR-friendly description of a local buffer: a pointer, length,
/// and L_Key. Users who manage MRs themselves (the most common case)
/// build `buffer_view`s directly; the optional `memory_region<Backend>`
/// RAII helper (S6) yields `buffer_view` from `.view()`.
struct buffer_view {
    void*       addr = nullptr;
    std::size_t length = 0;
    std::uint32_t lkey = 0;
};

/// Single scatter-gather entry. Until S5a backends typically receive a
/// single-element `std::span<const sge>` synthesised from a `buffer_view`;
/// S5a switches the public API to multi-segment spans.
struct sge {
    void*         addr = nullptr;
    std::uint32_t length = 0;
    std::uint32_t lkey = 0;

    /// Convenience: build an sge from a buffer_view.
    [[nodiscard]] static sge from(buffer_view v) noexcept {
        assert(v.length <= std::uint32_t(-1) &&
               "buffer_view::length exceeds uint32_t range for sge");
        return sge{v.addr, static_cast<std::uint32_t>(v.length), v.lkey};
    }
};

/// Description of a remote buffer for one-sided RDMA WRITE / READ.
struct remote_buffer {
    std::uint64_t addr = 0;     ///< Remote virtual address.
    std::uint32_t length = 0;   ///< Size of the remote region in bytes.
    std::uint32_t rkey = 0;     ///< R_Key the remote registered the MR with.
};

/// Normalised RDMA completion status. Maps the IBV_WC_* set onto a
/// portable enum so downstream code never has to include `<verbs.h>`.
/// The numeric values mirror IBV_WC_* for the dominant statuses, but
/// users should compare against the named enum constants, not integers.
enum class wc_status : std::uint32_t {
    success           = 0,
    local_length_error,
    local_qp_error,
    local_eec_error,
    local_protection_error,
    wr_flush_error,
    memory_window_bind_error,
    bad_response,
    local_access_error,
    remote_invalid_request,
    remote_access_error,
    remote_op_error,
    retry_exceeded,
    rnr_retry_exceeded,
    local_rdd_violation,
    remote_invalid_rd_request,
    remote_aborted,
    invalid_eecn,
    invalid_eec_state,
    fatal,
    response_timeout,
    general,
};

/// Returns true iff the status indicates the operation completed
/// successfully and any byte_len / imm_data fields in wc_result are
/// meaningful.
[[nodiscard]] constexpr bool is_success(wc_status s) noexcept {
    return s == wc_status::success;
}

/// Result returned by every RDMA awaiter (S3+). Mirrors the subset of
/// ibv_wc that user code typically cares about; backend code is free to
/// surface vendor-specific WC fields through extension types layered on
/// top of this.
struct wc_result {
    wc_status     status   = wc_status::success;
    std::uint32_t byte_len = 0;     ///< Bytes received/transferred.
    std::uint32_t imm_data = 0;     ///< Immediate data (if wc_flags & with_imm).
    std::uint32_t wc_flags = 0;     ///< Backend-defined flags (e.g. with_imm).

    [[nodiscard]] constexpr bool ok() const noexcept {
        return is_success(status);
    }
};

/// Result returned by atomic awaiters (S15: CAS / FAA). Wraps the
/// standard `wc_result` and carries the local buffer pointer so
/// callers don't have to thread it through manually to read the old
/// value the remote held BEFORE the atomic completed.
///
/// IB ATOMIC operations deliver the old value as 8 raw bytes
/// big-endian on the wire. `old_value_host()` does the `be64toh`
/// conversion; `old_value_raw()` returns the raw 8 bytes verbatim
/// (useful for backends with custom endianness such as mlx5's
/// `IBV_EXP_ATOMIC_HCA_REPLY_BE`, where the wire format isn't
/// strictly IB-canonical big-endian).
struct atomic_result {
    wc_result   wc{};
    buffer_view local{};

    [[nodiscard]] constexpr bool ok() const noexcept { return wc.ok(); }

    [[nodiscard]] std::uint64_t old_value_host() const noexcept {
        std::uint64_t be = 0;
        if (local.addr && local.length >= sizeof(be)) {
            std::memcpy(&be, local.addr, sizeof(be));
        }
        return be64toh(be);
    }

    [[nodiscard]] std::uint64_t old_value_raw() const noexcept {
        std::uint64_t raw = 0;
        if (local.addr && local.length >= sizeof(raw)) {
            std::memcpy(&raw, local.addr, sizeof(raw));
        }
        return raw;
    }
};

/// Per-op modifiers. Bit-set style so the API can grow without breaking
/// signatures; backends mask out bits they don't support.
///
/// `signaled`     — request a completion (CQE). Most production WRs set
///                  this; backends are free to suppress CQEs for unset
///                  WRs to reduce CQ pressure. Awaited connection
///                  operations force this on so they can resume.
///                  Default ON.
/// `solicited`    — set IBV_SEND_SOLICITED (peer's CQ event fires on
///                  this WR's reception). Two-sided messaging hint.
/// `inline_send`  — request inline send (S5b); valid only when
///                  payload <= connection's max_inline_data.
/// `fence`        — execute after all prior WRs (IBV_SEND_FENCE).
/// `with_imm`     — request SEND_WITH_IMM or RDMA_WRITE_WITH_IMM
///                  (S11). Backend uses the imm_data parameter passed
///                  alongside flags. Awaitable's convenience methods
///                  `connection::send_with_imm` /
///                  `connection::rdma_write_with_imm` set this bit
///                  automatically.
struct send_flags {
    bool signaled    : 1 = true;
    bool solicited   : 1 = false;
    bool inline_send : 1 = false;
    bool fence       : 1 = false;
    bool with_imm    : 1 = false;

    [[nodiscard]] constexpr static send_flags none() noexcept {
        return send_flags{.signaled = false};
    }
};

}  // namespace elio::rdma
