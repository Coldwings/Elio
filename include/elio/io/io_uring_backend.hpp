#pragma once

#include "io_backend.hpp"
#include <elio/log/macros.hpp>
#include <elio/coro/vthread_stack.hpp>
#include <elio/coro/promise_base.hpp>
#include <elio/coro/frame.hpp>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <vector>

namespace elio::io {

// ---------------------------------------------------------------------------
// io_uring user_data tagging scheme
// ---------------------------------------------------------------------------
//
// Every SQE submitted via this backend stores a ``user_data`` value that the
// kernel echoes back on the matching CQE. The low two bits of that value are
// used as a discriminator so the completion handler can dispatch without any
// extra bookkeeping or locking. Because every dispatched pointer is at least
// 4-byte aligned, those bits are free to repurpose.
//
//   * ``bit 0 == 1``: tagged pointer to a ``batch_completion`` trampoline.
//     Mask off bit 0 to recover the pointer. Each trampoline knows which
//     segment of which ``batch_state`` it belongs to and is responsible for
//     delivering the per-segment result and, on the last completion of the
//     batch, resuming the awaiting coroutine.
//
//   * ``bit 1 == 1`` (with bit 0 clear): tagged pointer to an ``op_state``
//     allocated and owned by the issuing awaitable. Mask off bit 1 to
//     recover the pointer. The completion handler atomically transitions
//     the op_state from ``pending`` to either ``completed`` (it won the
//     race; resume the handle) or detects ``orphaned`` (the awaitable was
//     destroyed first; just delete the state). This guards against
//     resuming a freed coroutine frame after forced cancellation /
//     vthread teardown.
//
//   * ``bit 0 == 0`` and ``bit 1 == 0``: legacy raw coroutine handle. Used
//     by callers (e.g. timer, net, signal) that have not yet migrated to
//     ``op_state``. ``process_completion`` resumes the handle directly.
//
// A few sentinel values do not follow either convention. They are never
// dereferenced and are recognised by exact comparison:
//
//   * ``WAKE_SENTINEL`` (= 1): used by the eventfd poll SQE that wakes the
//     ring on cross-thread ``notify()``. Note this also has bit 0 set, but
//     is handled before the tag check.
//   * ``nullptr``: used for cancel SQEs that do not need to resume anything.
//
// ---------------------------------------------------------------------------
// pending_ops_ accounting invariant
// ---------------------------------------------------------------------------
//
// Every SQE submitted via this backend gets exactly one ``pending_ops_``
// increment on the way in and exactly one matching decrement when its CQE is
// processed. There is exactly one exception: the wake-eventfd poll SQE
// (``WAKE_SENTINEL``) is never counted on either side — it is purely
// internal plumbing and ``process_completion`` returns early before touching
// ``pending_ops_``.
//
// Cancel SQEs are slightly subtle but deliberately balanced: ``cancel()``
// adds one (line where ``pending_ops_.fetch_add(1)`` lives) and the
// completion of that cancel SQE arrives with ``user_data == nullptr``,
// hits the regular ``fetch_sub(1)`` in ``process_completion``, then bails
// out without resuming an awaiter. So pending_ops_ transiently rises by one
// per cancel and then falls back. ``run_until_complete()`` consequently
// waits for cancel CQEs as well as the original op's -ECANCELED CQE, which
// is fine — both arrive promptly.
//
// Why not just skip counting the cancel SQE? Because the ``submit()``
// rollback path (introduced in PR #58 to recover from io_uring_submit
// failures) rolls back ``io_uring_sq_ready()`` from ``pending_ops_``, which
// includes every staged SQE. Excluding cancels there would either cause an
// unsigned underflow on submit failure or require a parallel "internal SQE"
// counter, both of which add complexity to an error path. The
// every-SQE-counted scheme keeps the rollback math trivial.
//
// Net effect: ``pending_count()`` reports user ops in flight plus any
// in-flight cancel SQEs. Once a cancel completes, the count returns to
// exactly the number of user ops in flight.
// ---------------------------------------------------------------------------

struct batch_state;

/// Tag bits used in ``user_data`` (see scheme docs above).
inline constexpr uintptr_t USER_DATA_BATCH_TAG = 1;       ///< bit 0
inline constexpr uintptr_t USER_DATA_OP_STATE_TAG = 2;    ///< bit 1
inline constexpr uintptr_t USER_DATA_TAG_MASK = 3;        ///< bits 0..1

/// Per-segment trampoline used as tagged user_data on batch SQEs.
/// One ``batch_completion`` exists per segment; it lives inside the owning
/// ``batch_state::trampolines`` vector so its lifetime covers every CQE.
struct batch_completion {
    batch_state* state = nullptr; ///< Back-pointer to the shared batch state
    uint32_t segment_index = 0;   ///< Which results[i] this CQE updates

    /// Encode this trampoline into a tagged user_data value (low bit = 1).
    void* tagged_user_data() noexcept {
        return reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(this) | USER_DATA_BATCH_TAG);
    }
};

/// Encode an ``op_state*`` into a tagged user_data value (bit 1 = 1).
/// op_state is heap-allocated so it has at least 4-byte alignment, leaving
/// bits 0..1 free.
inline void* tagged_op_state_user_data(op_state* st) noexcept {
    return reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(st) | USER_DATA_OP_STATE_TAG);
}

/// Shared state for a batch I/O operation.
/// Kept here so both the io_uring backend (which dispatches CQEs) and the
/// batch awaitables in ``io_awaitables.hpp`` (which build trampolines) can
/// see the layout without a forward-declaration dance. Used by both the
/// io_uring path (per-CQE updates) and the epoll/synchronous fallback (which
/// just fills ``results`` directly).
///
/// Lifetime: ``batch_state`` is heap-allocated (via unique_ptr) on the
/// awaitable. An orphan protocol analogous to ``op_state`` protects against
/// UAF when the awaiting coroutine is destroyed while SQEs are in flight:
/// the destructor CAS's ``phase`` from ``phase_pending`` to ``phase_orphaned``,
/// releasing ownership. The final CQE handler sees ``phase_orphaned`` and
/// deletes the state instead of resuming a dangling awaiter.
struct batch_state {
    static constexpr uint8_t phase_pending = 0;
    static constexpr uint8_t phase_orphaned = 1;
    static constexpr uint8_t phase_completed = 2;

    std::atomic<int> completed{0};                 ///< Per-CQE counter
    int total = 0;                                 ///< Total segments
    std::vector<int> results;                      ///< Per-segment results (bytes or -errno)
    std::vector<batch_completion> trampolines;     ///< One trampoline per segment (io_uring only)
    std::coroutine_handle<> awaiter{};             ///< Resumed when completed == total
    coro::vthread_stack* awaiter_vstack = nullptr; ///< Captured at suspend time
    std::atomic<uint8_t> phase{phase_pending};     ///< Orphan protocol phase

    explicit batch_state(int n) : total(n), results(n) {}

    bool all_done() const noexcept {
        return completed.load(std::memory_order_acquire) >= total;
    }
};

} // namespace elio::io

#if ELIO_HAS_IO_URING

#include <liburing.h>
#include <sys/eventfd.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <thread>

namespace elio::io {

class io_uring_backend;

/// io_uring backend implementation
/// 
/// Provides high-performance asynchronous I/O on Linux 5.1+
/// Features:
/// - Submission queue batching
/// - Zero-copy operations (when available)
/// - Efficient completion handling
class io_uring_backend final : public io_backend {
public:
    /// Configuration options
    struct config {
        /// SQ/CQ ring depth (per worker), clamped to [64, 4096].
        /// 256 comfortably handles hundreds of concurrent connections;
        /// if the SQ fills up the backend flushes and retries, so a
        /// smaller ring only costs an extra submit syscall.
        uint32_t queue_depth = 256;
        uint32_t flags = 0;             ///< io_uring_setup flags
        bool sq_poll = false;           ///< Enable SQ polling (requires privileges)
    };
    
    /// Constructor with default configuration
    io_uring_backend() : io_uring_backend(config{}) {}
    
    /// Constructor with custom configuration
    explicit io_uring_backend(const config& cfg)
        : pending_ops_(0) {

        const uint32_t depth = std::clamp(cfg.queue_depth, 64u, 4096u);

        struct io_uring_params params = {};
        params.flags = cfg.flags;

        if (cfg.sq_poll) {
            params.flags |= IORING_SETUP_SQPOLL;
            params.sq_thread_idle = 2000;  // 2 second idle timeout
        }

        int ret = io_uring_queue_init_params(depth, &ring_, &params);
        if (ret < 0) {
            throw std::runtime_error(
                std::string("io_uring_queue_init failed: ") + strerror(-ret)
            );
        }
        
        ELIO_LOG_INFO("io_uring_backend initialized (queue_depth={}, flags=0x{:x})",
                      depth, params.flags);

        // Create eventfd for cross-thread wake notifications
        wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd_ < 0) {
            io_uring_queue_exit(&ring_);
            throw std::runtime_error(
                std::string("eventfd creation failed: ") + strerror(errno)
            );
        }

        // Submit initial poll_add to watch wake_fd_
        submit_wake_poll();
        io_uring_submit(&ring_);
    }
    
    /// Destructor
    ~io_uring_backend() override {
        // Brief wait for pending operations - don't block forever
        // io_uring_queue_exit will handle cleanup of any remaining ops
        int retries = 10;  // Max ~100ms wait
        while (pending_ops_.load(std::memory_order_relaxed) > 0 && retries-- > 0) {
            poll(std::chrono::milliseconds(10));
        }
        
        if (pending_ops_.load(std::memory_order_relaxed) > 0) {
            ELIO_LOG_WARNING("io_uring_backend destroyed with {} pending operations",
                            pending_ops_.load(std::memory_order_relaxed));
        }
        
        if (wake_fd_ >= 0) {
            ::close(wake_fd_);
            wake_fd_ = -1;
        }

        io_uring_queue_exit(&ring_);
        ELIO_LOG_INFO("io_uring_backend destroyed");
    }
    
    // Non-copyable, non-movable
    io_uring_backend(const io_uring_backend&) = delete;
    io_uring_backend& operator=(const io_uring_backend&) = delete;
    io_uring_backend(io_uring_backend&&) = delete;
    io_uring_backend& operator=(io_uring_backend&&) = delete;
    
    /// Prepare an I/O operation
    bool prepare(const io_request& req) override {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            // SQ full: try to drain by submitting whatever is queued, then
            // retry once. This avoids spurious -EAGAIN at the awaitable layer
            // when the SQ is just busy and a flush is enough.
            ELIO_LOG_DEBUG("io_uring SQ full, flushing and retrying");
            (void)io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
            if (!sqe) {
                ELIO_LOG_WARNING("io_uring SQ still full after flush");
                return false;
            }
        }

        // Encode user_data: prefer op_state tagged pointer (UAF-safe path),
        // fall back to raw coroutine handle for legacy callers.
        if (req.state) {
            io_uring_sqe_set_data(sqe, tagged_op_state_user_data(req.state));
        } else {
            io_uring_sqe_set_data(sqe, req.awaiter.address());
        }
        
        switch (req.op) {
            case io_op::read:
                if (req.offset >= 0) {
                    io_uring_prep_read(sqe, req.fd, req.buffer, 
                                       static_cast<unsigned>(req.length), 
                                       static_cast<__u64>(req.offset));
                } else {
                    io_uring_prep_read(sqe, req.fd, req.buffer, 
                                       static_cast<unsigned>(req.length), 0);
                }
                break;
                
            case io_op::write:
                if (req.offset >= 0) {
                    io_uring_prep_write(sqe, req.fd, req.buffer, 
                                        static_cast<unsigned>(req.length), 
                                        static_cast<__u64>(req.offset));
                } else {
                    io_uring_prep_write(sqe, req.fd, req.buffer, 
                                        static_cast<unsigned>(req.length), 0);
                }
                break;
                
            case io_op::readv:
                io_uring_prep_readv(sqe, req.fd, req.iovecs, 
                                    static_cast<unsigned>(req.iovec_count),
                                    static_cast<__u64>(req.offset >= 0 ? req.offset : 0));
                break;
                
            case io_op::writev:
                io_uring_prep_writev(sqe, req.fd, req.iovecs, 
                                     static_cast<unsigned>(req.iovec_count),
                                     static_cast<__u64>(req.offset >= 0 ? req.offset : 0));
                break;
                
            case io_op::accept:
                io_uring_prep_accept(sqe, req.fd, req.addr, req.addrlen, 
                                     req.socket_flags);
                break;
                
            case io_op::connect:
                io_uring_prep_connect(sqe, req.fd, req.addr, 
                                      req.addrlen ? *req.addrlen : 0);
                break;
                
            case io_op::recv:
                io_uring_prep_recv(sqe, req.fd, req.buffer, req.length, 
                                   req.socket_flags);
                break;
                
            case io_op::send:
                io_uring_prep_send(sqe, req.fd, req.buffer, req.length, 
                                   req.socket_flags);
                break;
                
            case io_op::close:
                io_uring_prep_close(sqe, req.fd);
                break;
                
            case io_op::timeout: {
                // Timeout in nanoseconds
                // Store timespec in user_data area of the awaiter to avoid shared state
                // The awaiter must have a ts_ member for this to work
                auto ns = static_cast<int64_t>(req.length);
                auto* ts = static_cast<__kernel_timespec*>(req.timeout_ts);
                if (ts) {
                    ts->tv_sec = ns / 1000000000LL;
                    ts->tv_nsec = ns % 1000000000LL;
                    io_uring_prep_timeout(sqe, ts, 0, 0);
                }
                break;
            }
                
            case io_op::cancel:
                io_uring_prep_cancel(sqe, req.user_data, 0);
                break;
                
            case io_op::poll_read:
                io_uring_prep_poll_add(sqe, req.fd, POLLIN);
                break;
                
            case io_op::poll_write:
                io_uring_prep_poll_add(sqe, req.fd, POLLOUT);
                break;
                
            default:
                ELIO_LOG_ERROR("Unknown io_op: {}", static_cast<int>(req.op));
                return false;
        }
        
        pending_ops_.fetch_add(1, std::memory_order_relaxed);
        ELIO_LOG_DEBUG("Prepared io_op::{} on fd={}", 
                       static_cast<int>(req.op), req.fd);
        return true;
    }
    
    /// Submit all prepared operations
    int submit() override {
        // Fast path: if no pending submissions, skip syscall
        size_t pending = io_uring_sq_ready(&ring_);
        if (pending == 0) {
            return 0;
        }

        int submitted = io_uring_submit(&ring_);
        if (submitted < 0) {
            ELIO_LOG_ERROR("io_uring_submit failed: {}", strerror(-submitted));
            // Rollback pending_ops for the operations that failed to submit.
            // Without this rollback, pending_ops_ stays elevated forever and
            // has_pending() / poll() will spin or block indefinitely.
            pending_ops_.fetch_sub(pending, std::memory_order_relaxed);
            return submitted;
        }

        // If we submitted fewer than prepared (shouldn't happen with
        // io_uring_submit), rollback the excess.
        if (static_cast<size_t>(submitted) < pending) {
            size_t rollback = pending - static_cast<size_t>(submitted);
            pending_ops_.fetch_sub(rollback, std::memory_order_relaxed);
            ELIO_LOG_WARNING("io_uring_submit submitted {}/{} SQEs", submitted, pending);
        }

        ELIO_LOG_DEBUG("Submitted {} operations", submitted);
        return submitted;
    }
    
    /// Poll for completed operations
    int poll(std::chrono::milliseconds timeout) override {
        // Auto-submit any pending operations before polling
        // This enables batching: multiple prepares followed by one submit
        if (io_uring_sq_ready(&ring_) > 0) {
            int ret = io_uring_submit(&ring_);
            if (ret < 0) {
                // Log but do NOT rollback pending_ops_: poll() will retry
                // on the next iteration and a rollback here would cause
                // underflow when the retried SQEs succeed and their CQEs
                // decrement pending_ops_.
                ELIO_LOG_ERROR("io_uring_submit in poll() failed: {}", strerror(-ret));
            }
        }

        struct io_uring_cqe* cqe = nullptr;
        int completions = 0;
        std::vector<deferred_resume_entry> deferred_resumes;
        
        if (timeout.count() == 0) {
            // Non-blocking: peek for available CQEs
            while (io_uring_peek_cqe(&ring_, &cqe) == 0 && cqe) {
                process_completion(cqe, &deferred_resumes);
                io_uring_cqe_seen(&ring_, cqe);
                completions++;
                cqe = nullptr;
            }
        } else if (timeout.count() < 0) {
            // Blocking: wait for at least one CQE
            int ret = io_uring_wait_cqe(&ring_, &cqe);
            if (ret == 0 && cqe) {
                process_completion(cqe, &deferred_resumes);
                io_uring_cqe_seen(&ring_, cqe);
                completions++;
                
                // Drain any additional ready CQEs
                cqe = nullptr;
                while (io_uring_peek_cqe(&ring_, &cqe) == 0 && cqe) {
                    process_completion(cqe, &deferred_resumes);
                    io_uring_cqe_seen(&ring_, cqe);
                    completions++;
                    cqe = nullptr;
                }
            }
        } else {
            // Timed wait
            struct __kernel_timespec ts;
            ts.tv_sec = timeout.count() / 1000;
            ts.tv_nsec = (timeout.count() % 1000) * 1000000;
            
            int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
            if (ret == 0 && cqe) {
                process_completion(cqe, &deferred_resumes);
                io_uring_cqe_seen(&ring_, cqe);
                completions++;
                
                // Drain any additional ready CQEs
                cqe = nullptr;
                while (io_uring_peek_cqe(&ring_, &cqe) == 0 && cqe) {
                    process_completion(cqe, &deferred_resumes);
                    io_uring_cqe_seen(&ring_, cqe);
                    completions++;
                    cqe = nullptr;
                }
            }
        }
        
        // Resume coroutines after processing all completions
        // Each coroutine gets its correct result via deferred_resume_entry
        resume_deferred(deferred_resumes);
        
        if (completions > 0) {
            ELIO_LOG_DEBUG("Processed {} completions", completions);
        }
        
        return completions;
    }
    
    /// Check if there are pending operations
    bool has_pending() const noexcept override {
        return pending_ops_.load(std::memory_order_relaxed) > 0;
    }
    
    /// Get the number of pending operations
    size_t pending_count() const noexcept override {
        return pending_ops_.load(std::memory_order_relaxed);
    }
    
    /// Submit a fire-and-forget ``IORING_OP_CLOSE`` for ``fd``.
    ///
    /// Use this from non-coroutine destructors that cannot ``co_await`` a
    /// regular ``async_close`` awaitable. The kernel orders ``IORING_OP_CLOSE``
    /// after every earlier-submitted SQE on the same fd that is in flight on
    /// this ring, so a concurrent in-flight ``recv``/``send``/``poll`` cannot
    /// end up operating on a recycled fd.
    ///
    /// The CQE arrives with ``user_data == nullptr`` and is silently consumed
    /// by ``process_completion``; the matching ``pending_ops_`` increment we
    /// add here is symmetrically decremented there, preserving the every-SQE-
    /// counted invariant documented at the top of this file.
    ///
    /// @return true if the SQE was queued; false if both the initial
    ///         ``io_uring_get_sqe`` and the post-flush retry returned null.
    ///         On false the caller should fall back to ``::close``.
    /// @note Must be called from the thread that owns this io_uring (the
    ///       worker thread). io_uring SQE production is not thread-safe.
    bool submit_close_async(int fd) noexcept {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            (void)io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
            if (!sqe) {
                return false;
            }
        }
        io_uring_prep_close(sqe, fd);
        // Fire-and-forget: nullptr user_data lands in process_completion's
        // "no awaiter to resume" branch (same path as cancel SQE completions).
        io_uring_sqe_set_data(sqe, nullptr);
        pending_ops_.fetch_add(1, std::memory_order_relaxed);
        // Submit immediately: the caller (a destructor) does not poll on its
        // own. Other workers will eventually drain the CQE; if no one ever
        // does, ``io_uring_queue_exit`` cleans up at backend teardown.
        (void)io_uring_submit(&ring_);
        return true;
    }

    /// Cancel a pending operation
    /// @param user_data The exact ``user_data`` value the original SQE was
    ///                  submitted with. For the legacy (raw handle) path
    ///                  this is ``coroutine_handle::address()``; for the
    ///                  ``op_state`` path the awaitable must pass the
    ///                  tagged value via ``tagged_op_state_user_data``.
    bool cancel(void* user_data) override {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            // Try a flush + retry, mirroring prepare(). A cancel that
            // can't be queued is silently dropped (caller will likely
            // observe normal completion or be torn down).
            (void)io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
            if (!sqe) {
                return false;
            }
        }

        io_uring_prep_cancel(sqe, user_data, 0);
        io_uring_sqe_set_data(sqe, nullptr);  // No awaiter for cancel itself

        // Count the cancel SQE itself in pending_ops_ so the submit()
        // rollback path can use io_uring_sq_ready() without an extra
        // "internal SQE" counter (see invariant block at top of file).
        // The matching CQE arrives with nullptr user_data and is decremented
        // back out by process_completion, restoring the count.
        pending_ops_.fetch_add(1, std::memory_order_relaxed);

        // Stage only — the next poll() auto-submits. Cancellation latency
        // is bounded by the poll loop's wakeup, which happens within
        // microseconds on a busy worker. Skipping the immediate submit
        // saves a syscall on every cancel.
        return true;
    }
    
    /// Check if io_uring is available on this system
    static bool is_available() noexcept {
        struct io_uring ring;
        int ret = io_uring_queue_init(1, &ring, 0);
        if (ret == 0) {
            io_uring_queue_exit(&ring);
            return true;
        }
        return false;
    }

    /// Override to indicate this is an io_uring backend
    bool is_io_uring() const noexcept override { return true; }

    /// Get direct access to the underlying io_uring ring.
    /// Used by batch I/O operations to prepare multiple SQEs at once.
    /// @return Pointer to the io_uring struct, or nullptr if not initialized
    struct io_uring* get_ring() noexcept {
        return &ring_;
    }

    /// Account for ``n`` SQEs that were prepared via ``get_ring()`` outside
    /// the regular ``prepare()`` path (e.g. by the batch I/O awaitables).
    /// Each registered op must produce exactly one CQE, which the backend
    /// will then count as completed via ``process_completion``.
    void register_pending(size_t n) noexcept {
        if (n) {
            pending_ops_.fetch_add(n, std::memory_order_relaxed);
        }
    }

    /// Rollback pending_ops_ for SQEs that failed to submit.
    void unregister_pending(size_t n) noexcept {
        if (n) {
            pending_ops_.fetch_sub(n, std::memory_order_relaxed);
        }
    }

    /// Resume a coroutine handle while temporarily setting its
    /// ``vthread_stack`` as current. Exposed for the batch trampoline to
    /// reuse the same logic the backend uses internally.
    static void resume_with_vstack(std::coroutine_handle<> handle) {
        safe_resume(handle);
    }

    void notify() override {
        uint64_t val = 1;
        ssize_t ret = ::write(wake_fd_, &val, sizeof(val));
        if (ret < 0 && errno != EAGAIN) {
            ELIO_LOG_WARNING("eventfd write failed: {}", strerror(errno));
        }
    }

    void drain_notify() override {
        uint64_t val;
        ssize_t ret = ::read(wake_fd_, &val, sizeof(val));
        if (ret < 0 && errno != EAGAIN) {
            ELIO_LOG_WARNING("eventfd read failed: {}", strerror(errno));
        }
    }

private:
    /// Deferred resume entry - stores handle with its result
    struct deferred_resume_entry {
        std::coroutine_handle<> handle;
        io_result result;
    };
    
    void process_completion(struct io_uring_cqe* cqe,
                           std::vector<deferred_resume_entry>* deferred_resumes = nullptr) {
        void* user_data = io_uring_cqe_get_data(cqe);

        // Wake notification: never counted in pending_ops_ (see the invariant
        // block at the top of this file). Must be checked before any tag-bit
        // check because WAKE_SENTINEL itself has bit 0 set.
        if (user_data == reinterpret_cast<void*>(WAKE_SENTINEL)) {
            drain_notify();
            submit_wake_poll();
            return;
        }

        // Every other CQE pairs 1:1 with an SQE that incremented pending_ops_
        // (real user ops via prepare()/register_pending(), and cancel SQEs
        // via cancel()). Decrement here once, then dispatch.
        pending_ops_.fetch_sub(1, std::memory_order_relaxed);

        if (!user_data) {
            // Cancel SQE completion: no awaiter to resume. The matching
            // user op's CQE arrives separately with -ECANCELED through one
            // of the dispatch paths below.
            return;
        }

        // Check for error conditions (negative res indicates error)
        if (cqe->res < 0) {
            ELIO_LOG_DEBUG("Completing operation with error: result={}, flags={}",
                           cqe->res, cqe->flags);
        } else {
            ELIO_LOG_DEBUG("Completing operation: result={}, flags={}",
                           cqe->res, cqe->flags);
        }

        uintptr_t v = reinterpret_cast<uintptr_t>(user_data);
        if (v & USER_DATA_BATCH_TAG) {
            // Tagged pointer: batch_completion trampoline.
            auto* tramp = reinterpret_cast<batch_completion*>(v & ~USER_DATA_BATCH_TAG);
            dispatch_batch_completion(tramp, cqe->res, deferred_resumes);
            return;
        }
        if (v & USER_DATA_OP_STATE_TAG) {
            // Tagged pointer: owner-controlled op_state (UAF-safe path).
            auto* st = reinterpret_cast<op_state*>(v & ~USER_DATA_OP_STATE_TAG);
            dispatch_op_state(st, cqe->res, cqe->flags, deferred_resumes);
            return;
        }

        // Plain coroutine handle (legacy path): each prepare() sets a unique
        // awaiter and io_uring guarantees one CQE per SQE, so resuming
        // directly is safe — provided the awaiter frame is still alive.
        auto handle = std::coroutine_handle<>::from_address(user_data);
        if (!handle) {
            return;
        }

        io_result result{cqe->res, cqe->flags};
        if (deferred_resumes) {
            deferred_resumes->push_back({handle, result});
        } else {
            last_result_ = result;
            safe_resume(handle);
        }
    }

    /// Dispatch a CQE matched to an awaitable-owned ``op_state``.
    /// CAS pending->completed: we won; stamp result/flags and resume.
    /// CAS fails because phase is orphaned: the awaitable was destroyed
    /// before this CQE arrived; just delete the storage and skip resume.
    static void dispatch_op_state(
        op_state* st,
        int32_t res,
        uint32_t flags,
        std::vector<deferred_resume_entry>* deferred_resumes) {
        if (!st) {
            return;
        }
        uint8_t expected = op_state::phase_pending;
        if (!st->phase.compare_exchange_strong(
                expected, op_state::phase_completed,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            // expected was phase_orphaned: awaitable already torn down.
            // The destructor released ownership to us; free it.
            delete st;
            return;
        }
        st->result = res;
        st->flags = flags;
        auto handle = st->handle;
        if (!handle) {
            return;
        }
        io_result result{res, flags};
        if (deferred_resumes) {
            deferred_resumes->push_back({handle, result});
        } else {
            last_result_ = result;
            safe_resume(handle);
        }
    }

    /// Dispatch a batch CQE to its per-segment trampoline.
    /// Writes the segment result, increments the completed counter, and on
    /// the final segment resumes the awaiting coroutine (deferred if a
    /// resume queue is provided so we don't resume inside CQE iteration).
    /// If the awaitable has been destroyed (phase == phase_orphaned), the
    /// final CQE frees the batch_state instead of resuming.
    static void dispatch_batch_completion(
        batch_completion* tramp,
        int32_t res,
        std::vector<deferred_resume_entry>* deferred_resumes) {
        if (!tramp || !tramp->state) {
            return;
        }
        batch_state* st = tramp->state;

        // Check orphan protocol: if the awaitable was destroyed while
        // SQEs were in flight, only write results (the vector is still
        // alive since we own it now) but do NOT resume the awaiter.
        bool orphaned = st->phase.load(std::memory_order_acquire)
                        == batch_state::phase_orphaned;

        const uint32_t idx = tramp->segment_index;
        if (!orphaned && idx < st->results.size()) {
            st->results[idx] = res;
        }
        int prev = st->completed.fetch_add(1, std::memory_order_acq_rel);
        if (prev + 1 != st->total) {
            return;
        }
        // Final segment.
        if (orphaned) {
            // Awaitable already torn down — free the state ourselves.
            delete st;
            return;
        }
        // Mark as completed so the destructor's CAS fails and unique_ptr cleans up
        st->phase.store(batch_state::phase_completed, std::memory_order_release);
        auto handle = st->awaiter;
        if (!handle) {
            return;
        }
        if (deferred_resumes) {
            deferred_resumes->push_back({handle, io_result{0, 0}});
        } else {
            safe_resume(handle);
        }
    }
    
    /// Safely resume a coroutine handle with proper vthread_stack context
    /// This ensures current_ is set to the coroutine's vstack before resume
    /// and restored afterwards, preventing vthread_stack corruption.
    static void safe_resume(std::coroutine_handle<> handle) {
        auto* promise = coro::get_promise_base(handle.address());
        auto* prev_vstack = coro::vthread_stack::current();
        if (promise) {
            coro::vthread_stack::set_current(promise->vstack());
        }
        handle.resume();
        coro::vthread_stack::set_current(prev_vstack);
    }

    /// Resume collected coroutine handles (call outside of lock)
    static void resume_deferred(std::vector<deferred_resume_entry>& entries) {
        for (auto& entry : entries) {
            // Check error conditions
            if (entry.result.result < 0) {
                ELIO_LOG_DEBUG("Resuming deferred with error: result={}",
                               entry.result.result);
            }

            // The handle was already claimed in process_completion,
            // so we just need to check if it's valid
            if (entry.handle) {
                last_result_ = entry.result;
                safe_resume(entry.handle);
            }
        }
    }

    /// Submit a poll_add SQE to watch the wake eventfd
    void submit_wake_poll() {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            // SQ is full - flush and retry once. This ensures the wake
            // poll is always installed, otherwise cross-thread notify()
            // will be lost and workers can deadlock.
            ELIO_LOG_WARNING("SQ full for wake poll, flushing and retrying");
            io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
            if (!sqe) {
                ELIO_LOG_ERROR("Failed to submit wake poll: SQ still full after flush");
                return;
            }
        }
        io_uring_prep_poll_add(sqe, wake_fd_, POLLIN);
        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(WAKE_SENTINEL));
        // Don't count this as a pending user operation - submit immediately
        io_uring_submit(&ring_);
    }
    
public:
    /// Get the last completion result (thread-local)
    /// Used by awaitables to retrieve their result
    static io_result get_last_result() noexcept {
        return last_result_;
    }
    
private:
    struct io_uring ring_;                     ///< io_uring instance
    std::atomic<size_t> pending_ops_;          ///< Number of pending operations
    int wake_fd_ = -1;  ///< eventfd for cross-thread wake-up
    /// Sentinel user_data to identify wake notifications in CQE
    static constexpr uintptr_t WAKE_SENTINEL = 1;

    static inline thread_local io_result last_result_{};
};

} // namespace elio::io

#else // !ELIO_HAS_IO_URING

namespace elio::io {

/// Stub implementation when io_uring is not available
class io_uring_backend final : public io_backend {
public:
    io_uring_backend() {
        throw std::runtime_error("io_uring not available on this system");
    }
    
    bool prepare(const io_request&) override { return false; }
    int submit() override { return -1; }
    int poll(std::chrono::milliseconds) override { return 0; }
    bool has_pending() const noexcept override { return false; }
    size_t pending_count() const noexcept override { return 0; }
    bool cancel(void*) override { return false; }
    
    static bool is_available() noexcept { return false; }
    static io_result get_last_result() noexcept { return {}; }

    void notify() override {}
    void drain_notify() override {}
};

} // namespace elio::io

#endif // ELIO_HAS_IO_URING
