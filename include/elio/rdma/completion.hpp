#pragma once

/// @file completion.hpp
/// @brief CQE → coroutine dispatcher (stage S1).
///
/// The dispatcher is the connective tissue between a CQ poller (whether
/// the default S7 `cq_pump` coroutine or user-supplied custom polling)
/// and the awaiters parked on individual RDMA operations.
///
/// At S1 the dispatcher is stateless: `deliver()` is a free-standing
/// completion handler that decodes `wr_id` back into an `op_state*`
/// and runs the lifecycle race described in `op_state.hpp`. S7 will
/// extend this class with `attach_completion_channel(...)` to bind a
/// completion channel fd to an `io_context`.
///
/// The dispatcher is intentionally **thread-agnostic**: `deliver()` can
/// be invoked from any thread, including a custom busy-poll thread
/// running outside any Elio scheduler. Coroutine resumption is routed
/// through `runtime::schedule_handle`, which already handles the
/// no-current-scheduler case by resuming synchronously on the caller's
/// thread (matches the semantics already used by sync primitives and
/// cancel_token paths).

#include <elio/rdma/op_state.hpp>
#include <elio/rdma/types.hpp>
#include <elio/runtime/scheduler.hpp>

#include <cassert>
#include <cstdint>

namespace elio::rdma {

class dispatcher {
public:
    dispatcher() noexcept = default;

    dispatcher(const dispatcher&) = delete;
    dispatcher& operator=(const dispatcher&) = delete;
    dispatcher(dispatcher&&) = delete;
    dispatcher& operator=(dispatcher&&) = delete;

    /// Encode an `op_state*` pointer as a `wr_id` suitable to pass into
    /// the backend's `post_*` calls.
    ///
    /// Tagging convention (S1): the wr_id is just the raw pointer value.
    /// S5c will repurpose the low bit to disambiguate SRQ vs QP-bound
    /// completion routing; alignment guarantees of `op_state` (≥ 8 byte)
    /// keep that bit available.
    [[nodiscard]] static wr_id make_wr_id(detail::op_state* op) noexcept {
        static_assert(alignof(detail::op_state) >= 2,
                      "op_state must be >= 2-byte aligned for wr_id low-bit tagging");
        return static_cast<wr_id>(reinterpret_cast<std::uintptr_t>(op));
    }

    /// Decode the inverse of `make_wr_id`.
    [[nodiscard]] static detail::op_state* decode_wr_id(wr_id id) noexcept {
        return reinterpret_cast<detail::op_state*>(
            static_cast<std::uintptr_t>(id));
    }

    /// Deliver a completion to the matching awaiter (or silently free
    /// the orphaned state). Safe to call from any thread, including
    /// inside a custom CQ-poll loop. noexcept.
    ///
    /// @param id        wr_id originally returned by `make_wr_id`.
    ///                  Treated as a fatal programming error if non-zero
    ///                  but not produced by `make_wr_id`; we don't
    ///                  defensively validate (the cost would be paid on
    ///                  every CQE).
    /// @param status    Normalised completion status.
    /// @param byte_len  Bytes transferred (for RECV / RDMA READ; 0 for
    ///                  SEND / RDMA WRITE).
    /// @param imm_data  Immediate value (when wc_flags carries the
    ///                  backend's IBV_WC_WITH_IMM equivalent).
    /// @param wc_flags  Backend-defined flag bits, surfaced verbatim
    ///                  into `wc_result::wc_flags`.
    void deliver(wr_id id, wc_status status, std::uint32_t byte_len,
                 std::uint32_t imm_data = 0,
                 std::uint32_t wc_flags = 0) noexcept
    {
        auto* op = decode_wr_id(id);
        if (op == nullptr) [[unlikely]] {
            return;  // null wr_id used by no-awaiter paths (e.g. cancel SQEs).
        }

        const wc_result result{status, byte_len, imm_data, wc_flags};
        auto expected = op->phase.load(std::memory_order_acquire);

        for (;;) {
            if (expected == detail::op_phase::posting) {
                // await_suspend is still executing post_*(). Publish
                // the result, but leave resumption to finalize_post_()
                // so the awaiter cannot be destroyed before
                // await_suspend returns.
                op->result = result;
                if (op->phase.compare_exchange_strong(
                        expected,
                        detail::op_phase::completed,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    return;
                }
                continue;
            }

            if (expected == detail::op_phase::pending) {
                op->result = result;
                if (!op->phase.compare_exchange_strong(
                        expected,
                        detail::op_phase::completed,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    continue;
                }

                // We own the race. Hand the coroutine off to the scheduler
                // (or resume inline if no scheduler is current). We do NOT
                // free `op` here — the awaiter still holds a
                // `unique_ptr<op_state>` and is responsible for the free
                // after it has consumed `op->result` via await_resume.
                //
                // When the awaiter's destructor eventually runs, try_orphan
                // observes `completed` and returns false, so the unique_ptr
                // retains ownership and frees `op` normally.
                auto handle = op->handle;
                runtime::schedule_handle(handle);
                return;
            }

            // The awaiter destructor already CASed pending → orphaned.
            // No coroutine to resume; we own the heap node now and must
            // free it. Assert the expected phase to catch duplicate CQEs
            // (deliver called twice for the same wr_id) in debug builds.
            assert(expected == detail::op_phase::orphaned &&
                   "deliver() on non-pending op_state (duplicate CQE?)");
            if (expected == detail::op_phase::orphaned) {
                delete op;
            }
            return;
        }
    }

    /// Awaiter destructor helper: attempt the inverse race.
    ///
    /// Returns true if this call won the race (state was `pending`,
    /// now `orphaned`); the dispatcher will eventually free `op` when
    /// the CQE arrives. The awaiter must NOT free `op` itself in this
    /// case (call `.release()` on the unique_ptr).
    ///
    /// Returns false if the operation was never started or the dispatcher
    /// already completed it; the awaiter retains ownership of `op` and its
    /// unique_ptr destructor frees it.
    [[nodiscard]] static bool try_orphan(detail::op_state* op) noexcept {
        if (op == nullptr) return false;
        auto expected = detail::op_phase::pending;
        return op->phase.compare_exchange_strong(
            expected,
            detail::op_phase::orphaned,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }
};

}  // namespace elio::rdma
