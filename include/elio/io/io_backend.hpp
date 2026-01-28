#pragma once

#include <memory>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <span>
#include <sys/uio.h>
#include <sys/socket.h>

namespace elio::io {

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
    poll_write    ///< Wait for socket to be writable
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

/// I/O operation request
struct io_request {
    io_op op;                           ///< Operation type
    int fd;                             ///< File descriptor
    void* buffer;                       ///< Buffer for single-buffer ops
    size_t length;                      ///< Buffer length
    int64_t offset;                     ///< File offset (-1 for current position)
    std::coroutine_handle<> awaiter;    ///< Coroutine to resume on completion
    void* user_data;                    ///< User data for tracking

    // For vectored I/O
    ::iovec* iovecs;
    size_t iovec_count;

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
    /// @return true if prepared successfully, false if queue is full
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
};

} // namespace elio::io
