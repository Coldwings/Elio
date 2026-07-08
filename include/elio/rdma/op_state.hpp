#pragma once

/// @file op_state.hpp
/// @brief UAF-safe completion routing primitive (stage S1).
///
/// Modelled on PR #69's io_uring `op_state` mechanism. The dispatcher
/// receives a `wr_id` (S1: raw `op_state*` pointer, S5c will tag the
/// low bit for SRQ routing) and races the awaiter's destructor for
/// ownership of the heap node:
///
///   * Each awaiter (S3+) holds a `std::unique_ptr<op_state>`, arms it
///     as `posting`, and posts its WR with
///     `wr_id = dispatcher::make_wr_id(state.get())`.
///   * Once post_* returns, the awaiter commits `posting → pending`.
///     On completion, the dispatcher decodes the wr_id, CASes
///     `pending → completed`, fills the `wc_result`, and resumes the
///     coroutine. The awaiter then frees the op_state after
///     await_resume consumes the result.
///   * If completion arrives inline before post_* returns, the
///     dispatcher records the result with `posting → completed` and
///     the awaiter resumes inline only after await_suspend returns.
///   * If the awaiter is destroyed before the CQE arrives (e.g. its
///     parent task was cancelled), the awaiter's destructor CASes
///     `pending → orphaned`. If that CAS wins, the dispatcher's later
///     CQE arrival sees `orphaned` and frees the state. If that CAS
///     loses, the dispatcher already completed it and the awaiter's
///     unique_ptr frees the state normally.
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

/// Four-phase state of an in-flight RDMA operation.
///
///   * `pending`   — posted and suspended; both the dispatcher and
///                   the awaiter destructor may transition out.
///   * `posting`   — await_suspend is still inside the backend
///                   post_* call. A completion may record the result
///                   here, but must not resume the coroutine yet.
///   * `completed` — dispatcher won the race (CQE arrived first) or
///                   post failed synchronously. The awaiter still owns
///                   the heap node and frees it after await_resume.
///   * `orphaned`  — awaiter destructor won the race (frame went
///                   away first). The dispatcher's later CQE will
///                   silently drop and free the node.
enum class op_phase : std::uint8_t {
    pending   = 0,
    posting   = 1,
    completed = 2,
    orphaned  = 3,
};

/// Heap node owned through the lifetime race described above.
///
/// Layout is deliberately small (one atomic byte + a coroutine handle
/// + a wc_result) so per-op allocator pressure stays low; if S3 profiling
/// shows it matters we can layer a pool on top in a follow-up PR.
struct op_state {
    static_assert(std::atomic<op_phase>::is_always_lock_free,
                  "op_phase must be lock-free for the CAS-based lifecycle race");

    std::atomic<op_phase> phase{op_phase::pending};
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
