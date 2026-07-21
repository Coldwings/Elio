#pragma once

/// @file op_state.hpp
/// @brief UAF-safe completion routing primitive (stage S1).
///
/// Modelled on PR #69's io_uring `op_state` mechanism. The dispatcher
/// receives a `wr_id` (S1: raw `op_state*` pointer, S5c will tag the
/// low bit for SRQ routing) and races the awaiter's destructor for
/// ownership of the heap node:
///
///   * Each awaiter (S3+) holds an `unstarted`
///     `std::unique_ptr<op_state>`, arms it as `posting`, and posts its WR with
///     `wr_id = dispatcher::make_wr_id(state.get())`.
///   * Lazy `co_await` commits `posting → pending` once post_* returns.
///     Eager `start()` commits `posting → posted` instead, because there
///     is not yet a coroutine handle to resume.
///   * On completion, the dispatcher decodes the wr_id, CASes
///     `pending → completed` and resumes the coroutine, or CASes
///     `posted → completed` and stores the result for a later await.
///     The awaiter frees the op_state after await_resume consumes the
///     result, or on destruction of the completed started handle.
///   * If completion arrives inline before post_* returns, the
///     dispatcher records the result with `posting → completed` and
///     the awaiter resumes inline only after await_suspend returns.
///   * If the awaiter is explicitly destroyed before the CQE arrives, or an
///     eager started handle is dropped before being awaited, its destructor
///     CASes `pending` or `posted` to `orphaned`. If that CAS wins, the
///     dispatcher's later CQE arrival sees `orphaned` and frees the state.
///     If completion has already scheduled a suspended coroutine, however,
///     destroying that frame before it consumes the result is unsupported and
///     fails closed: the queued handle cannot safely be revoked.
///
/// Net invariant: exactly one party (dispatcher or awaiter destructor)
/// frees the op_state, and the coroutine handle is resumed at most once.
///
/// This file is fully self-contained — no scheduler or backend
/// dependency.

#include <elio/rdma/types.hpp>

#include <atomic>
#include <coroutine>
#include <cstdint>

namespace elio::rdma::detail {

/// Five-phase state of a lazy RDMA operation.
///
///   * `unstarted` — the lazy awaitable has not posted a work request.
///                   Its unique_ptr remains the sole owner.
///   * `posting`   — await_suspend is still inside the backend
///                   post_* call. A completion may record the result
///                   here, but must not resume the coroutine yet.
///   * `posted`    — explicitly started, but not yet awaited; completion
///                   stores a result for a later await.
///   * `pending`   — posted and suspended with a coroutine handle; both
///                   the dispatcher and the awaiter destructor may
///                   transition out.
///   * `completed` — dispatcher won the race (CQE arrived first) or
///                   submission failed before suspension. The awaiter
///                   still owns the heap node and frees it after
///                   await_resume or destruction.
///   * `orphaned`  — awaiter destructor won the race (frame or started
///                   handle went away first). The dispatcher's later CQE
///                   will silently drop and free the node.
enum class op_phase : std::uint8_t {
    unstarted = 0,
    posting   = 1,
    posted    = 2,
    pending   = 3,
    completed = 4,
    orphaned  = 5,
};

/// Heap node owned through the lifetime race described above.
///
/// Layout is deliberately small (one atomic byte + a coroutine handle
/// + a wc_result) so per-op allocator pressure stays low; if S3 profiling
/// shows it matters we can layer a pool on top in a follow-up PR.
struct op_state {
    static_assert(std::atomic<op_phase>::is_always_lock_free,
                  "op_phase must be lock-free for the CAS-based lifecycle race");

    std::atomic<op_phase> phase{op_phase::unstarted};
    std::coroutine_handle<> handle{};
    wc_result result{};

    op_state() noexcept = default;
    op_state(const op_state&) = delete;
    op_state& operator=(const op_state&) = delete;
    op_state(op_state&&) = delete;
    op_state& operator=(op_state&&) = delete;
    ~op_state() = default;
};

}  // namespace elio::rdma::detail
