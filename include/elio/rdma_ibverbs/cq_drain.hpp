#pragma once

/// @file cq_drain.hpp
/// @brief Ready-made libibverbs drain callable for `elio::rdma::cq_pump`.
///
/// `cq_pump` is generic over a drain callable so it doesn't have to
/// link libibverbs. This header bridges the two: pass the returned
/// lambda straight into `cq_pump` and a CQ event on the channel fd
/// fans out to one or more `dispatcher.deliver(...)` calls — exactly
/// the boilerplate every ibverbs-backed app would otherwise write
/// themselves.
///
/// Sequence per wake:
///   1. `ibv_get_cq_event` on the channel (acks event count = 1).
///   2. `ibv_req_notify_cq(cq, 0)` to re-arm before polling so we
///      never miss a CQE that lands between poll and re-arm.
///   3. `ibv_poll_cq` in a loop until empty; each WC translated and
///      forwarded to `dispatcher.deliver`.
///
/// This matches the canonical libibverbs CQ-pump pattern; users who
/// need a different ack cadence (e.g. accumulating ack counts) can
/// write their own drain — the helper is purely convenience.

#include <elio/rdma/completion.hpp>
#include <elio/rdma/types.hpp>

#include <endian.h>
#include <infiniband/verbs.h>

namespace elio::rdma_ibverbs {

/// Map an `ibv_wc_status` onto Elio's normalised `wc_status`.
[[nodiscard]] inline elio::rdma::wc_status
translate_status(ibv_wc_status s) noexcept {
    using S = elio::rdma::wc_status;
    switch (s) {
        case IBV_WC_SUCCESS:               return S::success;
        case IBV_WC_LOC_LEN_ERR:           return S::local_length_error;
        case IBV_WC_LOC_QP_OP_ERR:         return S::local_qp_error;
        case IBV_WC_LOC_EEC_OP_ERR:        return S::local_eec_error;
        case IBV_WC_LOC_PROT_ERR:          return S::local_protection_error;
        case IBV_WC_WR_FLUSH_ERR:          return S::wr_flush_error;
        case IBV_WC_MW_BIND_ERR:           return S::memory_window_bind_error;
        case IBV_WC_BAD_RESP_ERR:          return S::bad_response;
        case IBV_WC_LOC_ACCESS_ERR:        return S::local_access_error;
        case IBV_WC_REM_INV_REQ_ERR:       return S::remote_invalid_request;
        case IBV_WC_REM_ACCESS_ERR:        return S::remote_access_error;
        case IBV_WC_REM_OP_ERR:            return S::remote_op_error;
        case IBV_WC_RETRY_EXC_ERR:         return S::retry_exceeded;
        case IBV_WC_RNR_RETRY_EXC_ERR:     return S::rnr_retry_exceeded;
        case IBV_WC_LOC_RDD_VIOL_ERR:      return S::local_rdd_violation;
        case IBV_WC_REM_INV_RD_REQ_ERR:    return S::remote_invalid_rd_request;
        case IBV_WC_REM_ABORT_ERR:         return S::remote_aborted;
        case IBV_WC_INV_EECN_ERR:          return S::invalid_eecn;
        case IBV_WC_INV_EEC_STATE_ERR:     return S::invalid_eec_state;
        case IBV_WC_FATAL_ERR:             return S::fatal;
        case IBV_WC_RESP_TIMEOUT_ERR:      return S::response_timeout;
        default:                           return S::general;
    }
}

/// Returns a callable suitable for `elio::rdma::cq_pump<Drain>` that
/// drains the given CQ when its comp channel becomes readable. The
/// lambda captures both pointers by value (cheap; both are non-null
/// stable handles), so the helper is safe to store / hand off.
[[nodiscard]] inline auto make_cq_drain(ibv_cq* cq,
                                        ibv_comp_channel* channel) {
    return [cq, channel](elio::rdma::dispatcher& disp) noexcept {
        ibv_cq* ev_cq    = nullptr;
        void*   ev_ctx   = nullptr;
        if (::ibv_get_cq_event(channel, &ev_cq, &ev_ctx) != 0) {
            return;  // EAGAIN-ish; just bail and let cq_pump re-poll
        }
        ::ibv_ack_cq_events(ev_cq, 1);
        (void)::ibv_req_notify_cq(cq, 0);
        ibv_wc wc{};
        while (::ibv_poll_cq(cq, 1, &wc) > 0) {
            disp.deliver(wc.wr_id, translate_status(wc.status),
                         wc.byte_len, be32toh(wc.imm_data), wc.wc_flags);
        }
    };
}

}  // namespace elio::rdma_ibverbs
