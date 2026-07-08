#pragma once

/// @file operations.hpp
/// @brief Data-path awaiters (send / recv / rdma_write / rdma_read).
///
/// Stage S3 landed the SEND / RECV awaiters; S4 added one-sided
/// WRITE / READ. S5a turned every awaiter into a multi-SGE-capable
/// shape: each accepts either a single `buffer_view` (built into one
/// inline SGE) or an arbitrary `std::span<const sge>` (a scatter list
/// the caller owns).
///
/// Each awaiter is parameterised on the backend type. Two backend
/// styles are supported via the `backend_invoker<B>` trait below:
///
///   * For a static-traits backend (any `B` satisfying
///     `backend_traits<B>`), `backend_invoker<B>::post_*` forwards to
///     `B::post_*(...)`. Zero runtime overhead.
///   * For the runtime-replaceable `polymorphic_backend`, the
///     specialisation forwards to the instance's virtual method via a
///     `polymorphic_backend*` carried in the awaiter.
///
/// Lifetime: every awaiter owns a `std::unique_ptr<op_state>` while
/// suspended. The op_state ↔ dispatcher race (see op_state.hpp +
/// completion.hpp) decides which side frees the node on the final
/// transition.
///
/// Multi-SGE lifetime: when an awaiter is constructed from a
/// `std::span<const sge>`, the underlying SGE array must outlive the
/// `co_await` expression. In practice the awaiter is a temporary
/// inside the caller's coroutine frame and the SGE array typically
/// lives in the same frame; the span is only dereferenced inside
/// `await_suspend` (i.e. while the caller is still in scope), so this
/// is the same lifetime requirement as the underlying buffer
/// payloads.

#include <elio/rdma/backend_traits.hpp>
#include <elio/rdma/completion.hpp>
#include <elio/rdma/op_state.hpp>
#include <elio/rdma/types.hpp>

#include <cassert>
#include <coroutine>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace elio::rdma::detail {

/// Trait that lets the same awaiter type drive both the static-traits
/// backend path and the polymorphic_backend path. Primary template uses
/// static dispatch; the specialisation for `polymorphic_backend` uses
/// the carried instance pointer.
template <typename Backend>
struct backend_invoker {
    static int post_send(void* qp,
                         std::span<const sge> sges,
                         send_flags flags,
                         std::uint32_t imm_data,
                         wr_id id,
                         [[maybe_unused]] Backend* backend) noexcept {
        return Backend::post_send(qp, sges, flags, imm_data, id);
    }
    static int post_recv(void* qp,
                         std::span<const sge> sges,
                         wr_id id,
                         [[maybe_unused]] Backend* backend) noexcept {
        return Backend::post_recv(qp, sges, id);
    }
    static int post_rdma_write(void* qp,
                               std::span<const sge> sges,
                               remote_buffer rb,
                               send_flags flags,
                               std::uint32_t imm_data,
                               wr_id id,
                               [[maybe_unused]] Backend* backend) noexcept {
        return Backend::post_rdma_write(qp, sges, rb, flags, imm_data, id);
    }
    static int post_rdma_read(void* qp,
                              std::span<const sge> sges,
                              remote_buffer rb,
                              wr_id id,
                              [[maybe_unused]] Backend* backend) noexcept {
        return Backend::post_rdma_read(qp, sges, rb, id);
    }
    // Only valid if Backend satisfies backend_with_srq<>. Constrained
    // at the call site by srq_recv_awaitable (S5c).
    static int post_srq_recv(void* srq_ptr,
                             std::span<const sge> sges,
                             wr_id id,
                             [[maybe_unused]] Backend* backend) noexcept {
        return Backend::post_srq_recv(srq_ptr, sges, id);
    }
    // Only valid if Backend satisfies backend_with_atomic<>;
    // constrained at the call site by rdma_cas_awaitable /
    // rdma_faa_awaitable.
    static int post_atomic_cas(void* qp,
                               std::span<const sge> sges,
                               remote_buffer rb,
                               std::uint64_t compare,
                               std::uint64_t swap,
                               send_flags flags,
                               wr_id id,
                               [[maybe_unused]] Backend* backend) noexcept {
        return Backend::post_atomic_cas(qp, sges, rb, compare, swap, flags, id);
    }
    static int post_atomic_fetch_add(void* qp,
                                     std::span<const sge> sges,
                                     remote_buffer rb,
                                     std::uint64_t add,
                                     send_flags flags,
                                     wr_id id,
                                     [[maybe_unused]] Backend* backend) noexcept {
        return Backend::post_atomic_fetch_add(qp, sges, rb, add, flags, id);
    }
};

template <>
struct backend_invoker<polymorphic_backend> {
    static int post_send(void* qp,
                         std::span<const sge> sges,
                         send_flags flags,
                         std::uint32_t imm_data,
                         wr_id id,
                         polymorphic_backend* backend) noexcept {
        return backend->post_send(qp, sges, flags, imm_data, id);
    }
    static int post_recv(void* qp,
                         std::span<const sge> sges,
                         wr_id id,
                         polymorphic_backend* backend) noexcept {
        return backend->post_recv(qp, sges, id);
    }
    static int post_rdma_write(void* qp,
                               std::span<const sge> sges,
                               remote_buffer rb,
                               send_flags flags,
                               std::uint32_t imm_data,
                               wr_id id,
                               polymorphic_backend* backend) noexcept {
        return backend->post_rdma_write(qp, sges, rb, flags, imm_data, id);
    }
    static int post_rdma_read(void* qp,
                              std::span<const sge> sges,
                              remote_buffer rb,
                              wr_id id,
                              polymorphic_backend* backend) noexcept {
        return backend->post_rdma_read(qp, sges, rb, id);
    }
    static int post_srq_recv(void* srq_ptr,
                             std::span<const sge> sges,
                             wr_id id,
                             polymorphic_backend* backend) noexcept {
        // polymorphic_backend's default returns -ENOTSUP for backends
        // that don't override; the awaiter surfaces that as a flush
        // error to the caller (same pattern as any other negative rc).
        return backend->post_srq_recv(srq_ptr, sges, id);
    }
    static int post_atomic_cas(void* qp,
                               std::span<const sge> sges,
                               remote_buffer rb,
                               std::uint64_t compare,
                               std::uint64_t swap,
                               send_flags flags,
                               wr_id id,
                               polymorphic_backend* backend) noexcept {
        return backend->post_atomic_cas(qp, sges, rb, compare, swap, flags, id);
    }
    static int post_atomic_fetch_add(void* qp,
                                     std::span<const sge> sges,
                                     remote_buffer rb,
                                     std::uint64_t add,
                                     send_flags flags,
                                     wr_id id,
                                     polymorphic_backend* backend) noexcept {
        return backend->post_atomic_fetch_add(qp, sges, rb, add, flags, id);
    }
};

/// Awaited send-queue operations must request a CQE to resume safely.
[[nodiscard]] constexpr send_flags require_completion_(send_flags flags) noexcept {
    flags.signaled = true;
    return flags;
}

[[nodiscard]] constexpr std::uint32_t byte_count_hint_(std::size_t bytes) noexcept {
    constexpr auto max = std::numeric_limits<std::uint32_t>::max();
    return bytes > max ? max : static_cast<std::uint32_t>(bytes);
}

/// Shared awaiter machinery — owns the op_state, runs the orphan race
/// in the destructor, exposes a typed `await_resume` returning the
/// `wc_result` filled in by the dispatcher.
///
/// Subclasses must implement `do_post()` returning the backend's int
/// status; on a non-zero return the awaiter synthesises a failed
/// `wc_result` (status = wr_flush_error) and resumes inline rather
/// than suspending.
class op_awaiter_base {
public:
    op_awaiter_base() noexcept
        : op_(std::make_unique<op_state>()) {}

    op_awaiter_base(const op_awaiter_base&) = delete;
    op_awaiter_base& operator=(const op_awaiter_base&) = delete;
    op_awaiter_base(op_awaiter_base&&) noexcept = default;
    op_awaiter_base& operator=(op_awaiter_base&&) noexcept = default;

    ~op_awaiter_base() noexcept {
        // If we still own `op_`, try to flip pending → orphaned so the
        // dispatcher's later CQE arrival frees the heap node instead of
        // us. If the dispatcher already completed (state == completed),
        // try_orphan returns false; the unique_ptr's destructor frees
        // the node — which is exactly the path that lets `await_resume`
        // return a valid `wc_result` snapshot.
        if (op_) {
            if (dispatcher::try_orphan(op_.get())) {
                // Dispatcher will take it from here.
                (void)op_.release();
            }
        }
    }

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    [[nodiscard]] wc_result await_resume() noexcept {
        // op_ is guaranteed live here (the awaiter destructor hasn't
        // run yet, and resumption only happens after the op reached
        // completed). Snapshot then return.
        return op_->result;
    }

protected:
    /// Called by derived class's await_suspend before posting the WR.
    /// Returns the wr_id to pass into the backend's post_* function.
    [[nodiscard]] wr_id arm_(std::coroutine_handle<> h) noexcept {
        op_->handle = h;
        // `posting` blocks an inline CQE from resuming the coroutine
        // until finalize_post_() knows post_* has returned.
        op_->phase.store(detail::op_phase::posting,
                         std::memory_order_release);
        return dispatcher::make_wr_id(op_.get());
    }

    /// If the backend reported a synchronous failure to post (rc != 0),
    /// synthesise a failed completion locally and return false so the
    /// awaiter resumes inline without suspending. Otherwise return true.
    [[nodiscard]] bool finalize_post_(int rc) noexcept {
        if (rc == 0) {
            auto expected = op_phase::posting;
            if (op_->phase.compare_exchange_strong(
                    expected,
                    op_phase::pending,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;  // suspend; CQE will arrive later
            }

            // A CQE arrived from inside post_*(). The dispatcher
            // already stored op_->result and marked the op completed;
            // return false so the coroutine resumes only after
            // await_suspend returns to the compiler-generated caller.
            assert(expected == op_phase::completed &&
                   "post completion escaped the posting/completed states");
            return false;
        }
        // Post failed at submission time. There will be no CQE. Write
        // the synthesised result and flip phase → completed so the
        // awaiter destructor's try_orphan returns false (no one will
        // ever deliver for this wr_id, so we must own the free here).
        // The unique_ptr in op_awaiter_base will free `op_` on
        // destruction.
        if (op_->phase.load(std::memory_order_acquire)
            == op_phase::completed) {
            return false;
        }
        op_->result = wc_result{
            .status   = wc_status::wr_flush_error,
            .byte_len = 0,
            .imm_data = static_cast<std::uint32_t>(-rc),  // pass back errno-ish
            .wc_flags = 0,
        };
        op_->phase.store(op_phase::completed,
                         std::memory_order_release);
        return false;  // do NOT suspend
    }

    /// Synthesise a precondition failure (no WR was posted), e.g. the
    /// inline-send size check rejecting an oversized payload. Same
    /// rationale as finalize_post_(rc != 0): no party will ever
    /// deliver a CQE, so the unique_ptr in op_awaiter_base must own
    /// the free.
    [[nodiscard]] bool fail_pre_post_(wc_status status,
                                      std::uint32_t hint = 0) noexcept {
        op_->result = wc_result{
            .status   = status,
            .byte_len = 0,
            .imm_data = hint,
            .wc_flags = 0,
        };
        op_->phase.store(op_phase::completed,
                         std::memory_order_release);
        return false;  // do NOT suspend
    }

private:
    std::unique_ptr<op_state> op_;
};

/// Tiny mixin that all four awaiters share: dual storage for either a
/// single inline SGE (built from a `buffer_view`) or a caller-owned
/// SGE span. `effective_sges_()` resolves to one or the other at the
/// point the WR is posted. Built inside `await_suspend`, never
/// stored; safe under awaiter moves.
class sge_holder {
public:
    explicit sge_holder(buffer_view buf) noexcept
        : inline_sge_(sge::from(buf)), external_sges_(),
          using_inline_(true) {}
    explicit sge_holder(std::span<const sge> sges) noexcept
        : inline_sge_{}, external_sges_(sges), using_inline_(false) {}

protected:
    [[nodiscard]] std::span<const sge> effective_sges_() const noexcept {
        return using_inline_
            ? std::span<const sge>(&inline_sge_, 1)
            : external_sges_;
    }

    /// Total bytes across the resolved SGE list. Used by S5b's inline
    /// send precondition check.
    [[nodiscard]] std::size_t total_bytes_() const noexcept {
        std::size_t total = 0;
        for (const auto& s : effective_sges_()) {
            total += s.length;
        }
        return total;
    }

private:
    sge                  inline_sge_;
    std::span<const sge> external_sges_;
    bool                 using_inline_;
};

/// SEND awaiter. Constructible from a single `buffer_view` (inline
/// SGE) or an `std::span<const sge>` (multi-segment WR).
///
/// S5b: when `flags.inline_send` is set, the awaiter validates the
/// total SGE bytes against `max_inline` (typically the owning
/// connection's `connection_config::max_inline_data`). If the payload
/// exceeds the limit the awaiter rejects the WR with `local_length_
/// error` BEFORE calling the backend — fail-fast saves a round-trip
/// to the backend's own check and gives users a clear precondition
/// failure.
template <typename Backend>
class send_awaitable : public op_awaiter_base, public sge_holder {
public:
    send_awaitable(void* qp,
                   Backend* backend_or_null,
                   buffer_view buf,
                   send_flags flags,
                   std::size_t max_inline = 0,
                   std::uint32_t imm_data = 0) noexcept
        : sge_holder(buf), qp_(qp), backend_(backend_or_null),
          flags_(require_completion_(flags)), max_inline_(max_inline),
          imm_data_(imm_data) {}

    send_awaitable(void* qp,
                   Backend* backend_or_null,
                   std::span<const sge> sges,
                   send_flags flags,
                   std::size_t max_inline = 0,
                   std::uint32_t imm_data = 0) noexcept
        : sge_holder(sges), qp_(qp), backend_(backend_or_null),
          flags_(require_completion_(flags)), max_inline_(max_inline),
          imm_data_(imm_data) {}

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) noexcept {
        const auto id = arm_(h);
        const auto total = total_bytes_();
        if (flags_.inline_send && total > max_inline_) {
            // Pass the offending byte count back via imm_data so the
            // caller can log it without re-walking the SGE list.
            return fail_pre_post_(
                wc_status::local_length_error,
                byte_count_hint_(total));
        }
        const int rc = backend_invoker<Backend>::post_send(
            qp_, effective_sges_(), flags_, imm_data_, id, backend_);
        return finalize_post_(rc);
    }

private:
    void*         qp_;
    Backend*      backend_;
    send_flags    flags_;
    std::size_t   max_inline_;
    std::uint32_t imm_data_;
};

/// RECV awaiter. Constructible from a single `buffer_view` or a
/// scatter list.
template <typename Backend>
class recv_awaitable : public op_awaiter_base, public sge_holder {
public:
    recv_awaitable(void* qp,
                   Backend* backend_or_null,
                   buffer_view buf) noexcept
        : sge_holder(buf), qp_(qp), backend_(backend_or_null) {}

    recv_awaitable(void* qp,
                   Backend* backend_or_null,
                   std::span<const sge> sges) noexcept
        : sge_holder(sges), qp_(qp), backend_(backend_or_null) {}

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) noexcept {
        const auto id = arm_(h);
        const int rc = backend_invoker<Backend>::post_recv(
            qp_, effective_sges_(), id, backend_);
        return finalize_post_(rc);
    }

private:
    void*    qp_;
    Backend* backend_;
};

/// One-sided RDMA WRITE awaiter. Pushes local payload (single buffer
/// or scatter list) to a remote buffer; no receive is consumed on the
/// peer side, but the local CQE still surfaces wr_flush_error /
/// remote_access_error / retry_exceeded on the usual failure modes.
///
/// Inline RDMA WRITE is supported with the same fail-fast precondition
/// as `send_awaitable` (see S5b).
template <typename Backend>
class rdma_write_awaitable : public op_awaiter_base, public sge_holder {
public:
    rdma_write_awaitable(void* qp,
                         Backend* backend_or_null,
                         buffer_view local,
                         remote_buffer remote,
                         send_flags flags,
                         std::size_t max_inline = 0,
                         std::uint32_t imm_data = 0) noexcept
        : sge_holder(local), qp_(qp), backend_(backend_or_null),
          remote_(remote), flags_(require_completion_(flags)),
          max_inline_(max_inline), imm_data_(imm_data) {}

    rdma_write_awaitable(void* qp,
                         Backend* backend_or_null,
                         std::span<const sge> locals,
                         remote_buffer remote,
                         send_flags flags,
                         std::size_t max_inline = 0,
                         std::uint32_t imm_data = 0) noexcept
        : sge_holder(locals), qp_(qp), backend_(backend_or_null),
          remote_(remote), flags_(require_completion_(flags)),
          max_inline_(max_inline), imm_data_(imm_data) {}

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) noexcept {
        const auto id = arm_(h);
        const auto total = total_bytes_();
        if (total > remote_.length) {
            return fail_pre_post_(
                wc_status::local_length_error,
                byte_count_hint_(total));
        }
        if (flags_.inline_send && total > max_inline_) {
            return fail_pre_post_(
                wc_status::local_length_error,
                byte_count_hint_(total));
        }
        const int rc = backend_invoker<Backend>::post_rdma_write(
            qp_, effective_sges_(), remote_, flags_, imm_data_, id, backend_);
        return finalize_post_(rc);
    }

private:
    void*         qp_;
    Backend*      backend_;
    remote_buffer remote_;
    send_flags    flags_;
    std::size_t   max_inline_;
    std::uint32_t imm_data_;
};

/// One-sided RDMA READ awaiter. Pulls remote bytes into the local
/// buffer (single or scatter list); on success wc_result.byte_len
/// reports the bytes received.
template <typename Backend>
class rdma_read_awaitable : public op_awaiter_base, public sge_holder {
public:
    rdma_read_awaitable(void* qp,
                        Backend* backend_or_null,
                        buffer_view local,
                        remote_buffer remote) noexcept
        : sge_holder(local), qp_(qp), backend_(backend_or_null),
          remote_(remote) {}

    rdma_read_awaitable(void* qp,
                        Backend* backend_or_null,
                        std::span<const sge> locals,
                        remote_buffer remote) noexcept
        : sge_holder(locals), qp_(qp), backend_(backend_or_null),
          remote_(remote) {}

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) noexcept {
        const auto id = arm_(h);
        const auto total = total_bytes_();
        if (total > remote_.length) {
            return fail_pre_post_(
                wc_status::local_length_error,
                byte_count_hint_(total));
        }
        const int rc = backend_invoker<Backend>::post_rdma_read(
            qp_, effective_sges_(), remote_, id, backend_);
        return finalize_post_(rc);
    }

private:
    void*         qp_;
    Backend*      backend_;
    remote_buffer remote_;
};

/// 8-byte ATOMIC compare-and-swap awaiter (S15). Single SGE; the
/// local buffer must be 8 bytes (where the OLD remote value lands).
/// `await_resume` returns `atomic_result` which wraps the standard
/// `wc_result` plus convenience accessors over the local buffer.
template <typename Backend>
class rdma_cas_awaitable : public op_awaiter_base {
public:
    rdma_cas_awaitable(void* qp,
                       Backend* backend_or_null,
                       buffer_view local,
                       remote_buffer remote,
                       std::uint64_t compare,
                       std::uint64_t swap,
                       send_flags flags) noexcept
        : qp_(qp), backend_(backend_or_null),
          local_(local), remote_(remote),
          compare_(compare), swap_(swap),
          flags_(require_completion_(flags)) {}

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) noexcept {
        const auto sge_val = sge::from(local_);
        auto sges = std::span<const sge>(&sge_val, 1);
        const auto id = arm_(h);
        const int rc = backend_invoker<Backend>::post_atomic_cas(
            qp_, sges, remote_, compare_, swap_, flags_, id, backend_);
        return finalize_post_(rc);
    }

    [[nodiscard]] atomic_result await_resume() noexcept {
        return atomic_result{
            .wc    = op_awaiter_base::await_resume(),
            .local = local_,
        };
    }

private:
    void*         qp_;
    Backend*      backend_;
    buffer_view   local_;
    remote_buffer remote_;
    std::uint64_t compare_;
    std::uint64_t swap_;
    send_flags    flags_;
};

/// 8-byte ATOMIC fetch-and-add awaiter (S15). Same shape as CAS;
/// the `add` value is added atomically at the remote, the OLD value
/// is delivered to the local buffer.
template <typename Backend>
class rdma_faa_awaitable : public op_awaiter_base {
public:
    rdma_faa_awaitable(void* qp,
                       Backend* backend_or_null,
                       buffer_view local,
                       remote_buffer remote,
                       std::uint64_t add,
                       send_flags flags) noexcept
        : qp_(qp), backend_(backend_or_null),
          local_(local), remote_(remote),
          add_(add), flags_(require_completion_(flags)) {}

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) noexcept {
        const auto sge_val = sge::from(local_);
        auto sges = std::span<const sge>(&sge_val, 1);
        const auto id = arm_(h);
        const int rc = backend_invoker<Backend>::post_atomic_fetch_add(
            qp_, sges, remote_, add_, flags_, id, backend_);
        return finalize_post_(rc);
    }

    [[nodiscard]] atomic_result await_resume() noexcept {
        return atomic_result{
            .wc    = op_awaiter_base::await_resume(),
            .local = local_,
        };
    }

private:
    void*         qp_;
    Backend*      backend_;
    buffer_view   local_;
    remote_buffer remote_;
    std::uint64_t add_;
    send_flags    flags_;
};

/// SRQ RECV awaiter (S5c). Posts to a shared receive queue rather
/// than a per-QP RQ. Completion arrives via whichever CQ the QP
/// consuming the WR is bound to; the dispatcher routes by op_state
/// pointer so the actual delivery path is identical to per-QP recv.
template <typename Backend>
class srq_recv_awaitable : public op_awaiter_base, public sge_holder {
public:
    srq_recv_awaitable(void* srq_ptr,
                       Backend* backend_or_null,
                       buffer_view buf) noexcept
        : sge_holder(buf), srq_(srq_ptr), backend_(backend_or_null) {}

    srq_recv_awaitable(void* srq_ptr,
                       Backend* backend_or_null,
                       std::span<const sge> sges) noexcept
        : sge_holder(sges), srq_(srq_ptr), backend_(backend_or_null) {}

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) noexcept {
        const auto id = arm_(h);
        const int rc = backend_invoker<Backend>::post_srq_recv(
            srq_, effective_sges_(), id, backend_);
        return finalize_post_(rc);
    }

private:
    void*    srq_;
    Backend* backend_;
};

}  // namespace elio::rdma::detail
