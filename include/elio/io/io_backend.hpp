#pragma once

#include "io_operation_guard.hpp"

#include <atomic>
#include <memory>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <span>
#include <utility>
#include <sys/uio.h>
#include <sys/socket.h>

namespace elio::io {

namespace detail {

template<typename Fn>
void run_noexcept(Fn&& fn) noexcept {
    try {
        std::forward<Fn>(fn)();
    } catch (...) {
        // Diagnostics must never invalidate backend-owned operation state.
    }
}

} // namespace detail

/// Owner-controlled op tag carried as ``user_data`` on submitted SQEs.
///
/// Lifetime / cancellation contract
/// --------------------------------
/// Each awaitable that submits an io_uring SQE allocates an ``op_state``
/// (``std::unique_ptr``) and passes its address through ``io_request::state``.
/// The backend tags that pointer (low bit 1 set, see io_uring_backend.hpp)
/// when storing it as the SQE's ``user_data`` so the completion handler can
/// distinguish it from a raw coroutine handle or a batch trampoline.
///
/// The kernel guarantees exactly one CQE per SQE. The lifecycle has three
/// phases tracked by the ``phase`` atomic:
///
///   * ``pending`` (0): SQE is in flight, awaitable still owns the state.
///   * ``completed`` (1): the CQE arrived first. Before publishing this
///     phase, ``process_completion`` has stamped ``result``/``flags`` and
///     copied every field it still needs, so the awaitable's ``unique_ptr``
///     may free the state on destruction.
///   * ``orphaned`` (2): the awaitable was destroyed before the CQE arrived
///     (e.g. forced ``coroutine_handle::destroy()`` or frame teardown). The
///     destructor releases ownership; ``process_completion`` sees the
///     orphaned phase, deletes the state, and skips the resume — preventing
///     the use-after-free on a freed coroutine frame.
///
/// CAS pending->completed (in process_completion) and CAS pending->orphaned
/// (in awaitable destructor) serialize the transfer of ownership between
/// the awaitable and the backend so neither side can free or resume after
/// the other has taken over.
struct op_state {
    enum : uint8_t {
        phase_pending = 0,
        phase_completed = 1,
        phase_orphaned = 2,
    };

    std::coroutine_handle<> handle{};
    std::atomic<uint8_t> phase{phase_pending};
    int32_t result = 0;
    uint32_t flags = 0;
    ::msghdr msg{};
    /// Retains worker/backend ownership until completion or orphan cleanup.
    detail::io_operation_guard operation_guard{};
};

/// I/O operation types
enum class io_op : uint8_t {
    none = 0,
    read,
    write,
    readv,
    writev,
    accept,
    connect,
    recv,
    send,
    close,
    timeout,
    cancel,
    poll_read,    ///< Wait for socket to be readable
    poll_write,   ///< Wait for socket to be writable
    sendmsg       ///< Scatter-gather socket send
};

/// I/O operation result
struct io_result {
    int32_t result;      ///< Bytes transferred or error code (negative = -errno)
    uint32_t flags;      ///< Backend-specific flags
    
    bool success() const noexcept {
        return result >= 0;
    }
    
    int bytes_transferred() const noexcept {
        return result >= 0 ? result : 0;
    }
    
    int error_code() const noexcept {
        return result < 0 ? -result : 0;
    }
};

namespace detail {

/// Completion slot shared by every backend on the current polling thread.
/// Legacy raw-handle awaitables read this immediately when they are resumed.
inline thread_local io_result last_completion_result{};

inline void set_last_completion_result(io_result result) noexcept {
    last_completion_result = result;
}

[[nodiscard]] inline io_result get_last_completion_result() noexcept {
    return last_completion_result;
}

} // namespace detail

/// I/O operation request
struct io_request {
    io_op op;                           ///< Operation type
    int fd;                             ///< File descriptor
    void* buffer;                       ///< Buffer for single-buffer ops
    size_t length;                      ///< Buffer length
    int64_t offset;                     ///< File offset (-1 for current position)
    std::coroutine_handle<> awaiter;    ///< Coroutine to resume on completion
    void* user_data;                    ///< User data for tracking
    /// Optional owner-controlled op tag. When non-null, io_uring_backend
    /// tags this pointer and uses it as the SQE's ``user_data`` instead of
    /// the raw awaiter handle, so a CQE arriving after the awaitable was
    /// destroyed is detected and dropped instead of resuming a freed frame.
    /// See ``op_state`` above.
    op_state* state = nullptr;

    // For vectored I/O
    ::iovec* iovecs;
    size_t iovec_count;
    ::msghdr* msg;

    // For socket operations
    ::sockaddr* addr;
    ::socklen_t* addrlen;
    int socket_flags;

    // For timeout operations - pointer to awaiter's local timespec to avoid data races
    // This is a void* to avoid including linux/time_types.h here
    void* timeout_ts;
};

/// Abstract I/O backend interface
/// Implementations: io_uring_backend, epoll_backend
class io_backend {
public:
    virtual ~io_backend() = default;

    /// Prepare an I/O operation (does not submit yet)
    /// @param req The I/O request to prepare
    /// @return true if prepared successfully, false if no operation was staged
    ///         because the queue is full or the backend rejected the request
    /// @throws Any exception is reported before the backend accepts the
    ///         request state; after accepting it, implementations must not
    ///         throw.
    virtual bool prepare(const io_request& req) = 0;

    /// Submit all prepared I/O operations
    /// @return Number of operations submitted
    virtual int submit() = 0;

    /// Poll for completed I/O operations
    /// @param timeout Maximum time to wait (-1 for infinite, 0 for non-blocking)
    /// @return Number of completions processed
    virtual int poll(std::chrono::milliseconds timeout) = 0;

    /// Check if there are pending operations
    virtual bool has_pending() const noexcept = 0;

    /// Get the number of pending operations
    virtual size_t pending_count() const noexcept = 0;

    /// Cancel a pending operation
    /// @param user_data The user_data of the operation to cancel
    /// @return true if cancellation was submitted
    virtual bool cancel(void* user_data) = 0;

    /// Check if this backend supports io_uring features (like timeout_ts)
    /// @return true if this is an io_uring backend
    virtual bool is_io_uring() const noexcept { return false; }

    /// Wake up a thread blocked in poll()
    /// Called from external threads to interrupt blocking I/O wait
    virtual void notify() noexcept = 0;

    /// Drain the wake notification (call after poll returns due to notify)
    /// This is a no-op if poll returned due to I/O completion
    virtual void drain_notify() = 0;
};

} // namespace elio::io
