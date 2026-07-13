#pragma once

#include "io_context.hpp"
#include <elio/log/macros.hpp>
#include <elio/coro/promise_base.hpp>
#include <elio/coro/frame.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/runtime/worker_thread.hpp>
#include <coroutine>
#include <cerrno>
#include <span>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>

namespace elio::io {

/// Get the io_context for the current worker thread
/// Falls back to default_io_context if not running in a worker
inline io_context& current_io_context() noexcept {
    auto* worker = runtime::worker_thread::current();
    if (worker) {
        return worker->io_context();
    }
    return default_io_context();
}

/// Base class for I/O awaitables
/// Provides common functionality for all async I/O operations.
///
/// Owns the ``op_state`` tag passed through ``io_request::state`` so that a
/// CQE arriving after the awaitable's frame has been freed (forced cancel /
/// vthread teardown) is dropped instead of resuming a dangling handle. See
/// the contract in io_backend.hpp.
class io_awaitable_base {
public:
    io_awaitable_base() noexcept = default;

    // Non-copyable, non-movable: op_state holds a pointer to the awaitable's
    // own coroutine handle and is referenced by an in-flight SQE. Copying or
    // moving would invalidate that pointer.
    io_awaitable_base(const io_awaitable_base&) = delete;
    io_awaitable_base& operator=(const io_awaitable_base&) = delete;
    io_awaitable_base(io_awaitable_base&&) = delete;
    io_awaitable_base& operator=(io_awaitable_base&&) = delete;

    ~io_awaitable_base() {
        if (!op_state_) {
            return;
        }
        // op_state_ is still owned: either the SQE was never submitted (in
        // which case it has phase_pending and no CQE will arrive — we just
        // free via unique_ptr below) OR the SQE is in flight and the CQE
        // hasn't been processed yet. The latter case is the UAF-prone one:
        // the awaitable's coroutine frame is being torn down while the
        // kernel still has a reference. CAS pending->orphaned to release
        // ownership; the eventual CQE will see phase_orphaned and delete.
        uint8_t expected = op_state::phase_pending;
        if (op_state_->phase.compare_exchange_strong(
                expected, op_state::phase_orphaned,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            // We won. The CQE handler will free the storage — release the
            // unique_ptr so we don't double-free. If the SQE was never
            // submitted (e.g. SQ-full path that already reset op_state_)
            // we never reach here.
            (void)op_state_.release();
            return;
        }
        // CAS failed: phase was already phase_completed. The CQE handler has
        // already copied everything it still needs from op_state_; unique_ptr
        // cleans up.
    }

    /// Never ready immediately - always suspend
    bool await_ready() const noexcept {
        return false;
    }

    /// Get the result of the I/O operation
    io_result result() const noexcept {
        return result_;
    }

protected:
    io_result result_{};
    size_t saved_affinity_ = coro::NO_AFFINITY;
    void* handle_address_ = nullptr;
    /// Owned op_state for the in-flight SQE. Nullable: lazily allocated by
    /// ``setup_op_state`` and reset on prepare-failure paths.
    std::unique_ptr<op_state> op_state_;

    /// Save current affinity and bind to current worker
    template<typename Promise>
    void bind_to_worker(std::coroutine_handle<Promise> handle) {
        handle_address_ = handle.address();
        if constexpr (std::is_base_of_v<coro::promise_base, Promise>) {
            saved_affinity_ = handle.promise().affinity();
            auto* worker = runtime::worker_thread::current();
            if (worker) {
                handle.promise().set_affinity(worker->worker_id());
            }
        }
    }

    /// Allocate the op_state and return a pointer suitable for
    /// ``io_request::state``. Stores the awaiter handle in op_state for
    /// the completion handler to resume.
    op_state* setup_op_state(std::coroutine_handle<> awaiter) {
        op_state_ = std::make_unique<op_state>();
        op_state_->handle = awaiter;
        return op_state_.get();
    }

    /// Drop op_state ownership without orphaning (used on prepare-failure
    /// paths where no SQE was submitted, so no CQE will arrive).
    void clear_op_state() noexcept {
        op_state_.reset();
    }

    /// Read the io_result deposited by the completion handler. Falls back
    /// to ``result_`` (set inline by EAGAIN/error paths) when there is no
    /// op_state, e.g. after ``clear_op_state``.
    io_result read_result_from_op_state() const noexcept {
        if (op_state_) {
            return io_result{op_state_->result, op_state_->flags};
        }
        return result_;
    }

    /// Restore previous affinity (called from await_resume)
    void restore_affinity() {
        if (handle_address_) {
            auto* promise = coro::get_promise_base(handle_address_);
            if (promise) {
                if (saved_affinity_ == coro::NO_AFFINITY) {
                    promise->clear_affinity();
                } else {
                    promise->set_affinity(saved_affinity_);
                }
            }
        }
    }
};

/// Awaitable for async read operations
class async_read_awaitable : public io_awaitable_base {
public:
    async_read_awaitable(int fd, void* buffer, size_t length, 
                         int64_t offset = -1) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , buffer_(buffer)
        , length_(length)
        , offset_(offset) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();

        io_request req{};
        req.op = io_op::read;
        req.fd = fd_;
        req.buffer = buffer_;
        req.length = length_;
        req.offset = offset_;
        req.awaiter = awaiter;
        req.state = setup_op_state(awaiter);

        if (!ctx.prepare(req)) {
            clear_op_state();
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        // No explicit submit: poll() auto-submits at the top of its loop,
        // saving one syscall per op while still keeping latency bounded.
    }

    io_result await_resume() noexcept {
        result_ = read_result_from_op_state();
        restore_affinity();
        return result_;
    }

private:
    int fd_;
    void* buffer_;
    size_t length_;
    int64_t offset_;
};

/// Awaitable for async write operations
class async_write_awaitable : public io_awaitable_base {
public:
    async_write_awaitable(int fd, const void* buffer, 
                          size_t length, int64_t offset = -1) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , buffer_(buffer)
        , length_(length)
        , offset_(offset) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();

        io_request req{};
        req.op = io_op::write;
        req.fd = fd_;
        req.buffer = const_cast<void*>(buffer_);
        req.length = length_;
        req.offset = offset_;
        req.awaiter = awaiter;
        req.state = setup_op_state(awaiter);

        if (!ctx.prepare(req)) {
            clear_op_state();
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
    }

    io_result await_resume() noexcept {
        result_ = read_result_from_op_state();
        restore_affinity();
        return result_;
    }

private:
    int fd_;
    const void* buffer_;
    size_t length_;
    int64_t offset_;
};

/// Awaitable for async recv operations
class async_recv_awaitable : public io_awaitable_base {
public:
    async_recv_awaitable(int fd, void* buffer, size_t length,
                         int flags = 0) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , buffer_(buffer)
        , length_(length)
        , flags_(flags) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();

        io_request req{};
        req.op = io_op::recv;
        req.fd = fd_;
        req.buffer = buffer_;
        req.length = length_;
        req.socket_flags = flags_;
        req.awaiter = awaiter;
        req.state = setup_op_state(awaiter);

        if (!ctx.prepare(req)) {
            clear_op_state();
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
    }

    io_result await_resume() noexcept {
        result_ = read_result_from_op_state();
        restore_affinity();
        return result_;
    }

private:
    int fd_;
    void* buffer_;
    size_t length_;
    int flags_;
};

/// Awaitable for async send operations
class async_send_awaitable : public io_awaitable_base {
public:
    async_send_awaitable(int fd, const void* buffer, 
                         size_t length, int flags = 0) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , buffer_(buffer)
        , length_(length)
        , flags_(flags) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();

        io_request req{};
        req.op = io_op::send;
        req.fd = fd_;
        req.buffer = const_cast<void*>(buffer_);
        req.length = length_;
        req.socket_flags = flags_;
        req.awaiter = awaiter;
        req.state = setup_op_state(awaiter);

        if (!ctx.prepare(req)) {
            clear_op_state();
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
    }

    io_result await_resume() noexcept {
        result_ = read_result_from_op_state();
        restore_affinity();
        return result_;
    }

private:
    int fd_;
    const void* buffer_;
    size_t length_;
    int flags_;
};

/// Awaitable for async accept operations
class async_accept_awaitable : public io_awaitable_base {
public:
    async_accept_awaitable(int listen_fd, 
                           struct sockaddr* addr = nullptr,
                           socklen_t* addrlen = nullptr,
                           int flags = 0) noexcept
        : io_awaitable_base()
        , listen_fd_(listen_fd)
        , addr_(addr)
        , addrlen_(addrlen)
        , flags_(flags) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();

        io_request req{};
        req.op = io_op::accept;
        req.fd = listen_fd_;
        req.addr = addr_;
        req.addrlen = addrlen_;
        req.socket_flags = flags_;
        req.awaiter = awaiter;
        req.state = setup_op_state(awaiter);

        if (!ctx.prepare(req)) {
            clear_op_state();
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
    }

    io_result await_resume() noexcept {
        result_ = read_result_from_op_state();
        restore_affinity();
        return result_;
    }

    /// Convenience: get the accepted fd directly
    int accepted_fd() const noexcept {
        return result_.success() ? result_.result : -1;
    }

private:
    int listen_fd_;
    struct sockaddr* addr_;
    socklen_t* addrlen_;
    int flags_;
};

/// Awaitable for async connect operations
class async_connect_awaitable : public io_awaitable_base {
public:
    async_connect_awaitable(int fd, 
                            const struct sockaddr* addr,
                            socklen_t addrlen) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , addr_(addr)
        , addrlen_(addrlen) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();

        io_request req{};
        req.op = io_op::connect;
        req.fd = fd_;
        req.addr = const_cast<struct sockaddr*>(addr_);
        req.addrlen = &addrlen_;
        req.awaiter = awaiter;
        req.state = setup_op_state(awaiter);

        if (!ctx.prepare(req)) {
            clear_op_state();
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
    }

    io_result await_resume() noexcept {
        result_ = read_result_from_op_state();
        restore_affinity();
        return result_;
    }

private:
    int fd_;
    const struct sockaddr* addr_;
    socklen_t addrlen_;
};

/// Awaitable for async close operations
class async_close_awaitable : public io_awaitable_base {
public:
    async_close_awaitable(int fd) noexcept
        : io_awaitable_base()
        , fd_(fd) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();

        io_request req{};
        req.op = io_op::close;
        req.fd = fd_;
        req.awaiter = awaiter;
        req.state = setup_op_state(awaiter);

        if (!ctx.prepare(req)) {
            clear_op_state();
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
    }

    io_result await_resume() noexcept {
        result_ = read_result_from_op_state();
        restore_affinity();
        return result_;
    }

private:
    int fd_;
};

/// Awaitable for async readv operations (scatter-gather read)
class async_readv_awaitable : public io_awaitable_base {
public:
    async_readv_awaitable(int fd, struct iovec* iovecs,
                          size_t iovec_count) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , iovecs_(iovecs)
        , iovec_count_(iovec_count) {}

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();

        io_request req{};
        req.op = io_op::readv;
        req.fd = fd_;
        req.iovecs = iovecs_;
        req.iovec_count = iovec_count_;
        req.offset = -1;
        req.awaiter = awaiter;
        req.state = setup_op_state(awaiter);

        if (!ctx.prepare(req)) {
            clear_op_state();
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
    }

    io_result await_resume() noexcept {
        result_ = read_result_from_op_state();
        restore_affinity();
        return result_;
    }

private:
    int fd_;
    struct iovec* iovecs_;
    size_t iovec_count_;
};

/// Awaitable for async writev operations (scatter-gather write)
class async_writev_awaitable : public io_awaitable_base {
public:
    async_writev_awaitable(int fd, struct iovec* iovecs,
                           size_t iovec_count) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , iovecs_(iovecs)
        , iovec_count_(iovec_count) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();

        io_request req{};
        req.op = io_op::writev;
        req.fd = fd_;
        req.iovecs = iovecs_;
        req.iovec_count = iovec_count_;
        req.offset = -1;
        req.awaiter = awaiter;
        req.state = setup_op_state(awaiter);

        if (!ctx.prepare(req)) {
            clear_op_state();
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
    }

    io_result await_resume() noexcept {
        result_ = read_result_from_op_state();
        restore_affinity();
        return result_;
    }

private:
    int fd_;
    struct iovec* iovecs_;
    size_t iovec_count_;
};

/// Awaitable for poll (wait for socket readable/writable)
class async_poll_awaitable : public io_awaitable_base {
public:
    async_poll_awaitable(int fd, bool for_read) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , for_read_(for_read) {}
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);
        auto& ctx = current_io_context();

        io_request req{};
        req.op = for_read_ ? io_op::poll_read : io_op::poll_write;
        req.fd = fd_;
        req.awaiter = awaiter;
        req.state = setup_op_state(awaiter);

        if (!ctx.prepare(req)) {
            clear_op_state();
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
    }

    io_result await_resume() noexcept {
        result_ = read_result_from_op_state();
        restore_affinity();
        return result_;
    }

private:
    int fd_;
    bool for_read_;
};

/// Factory functions for creating awaitables

/// Create an async read awaitable
/// @param fd File descriptor to read from
/// @param buffer Buffer to read into
/// @param length Number of bytes to read
/// @param offset File offset (-1 for current position)
inline auto async_read(int fd, void* buffer, size_t length, 
                       int64_t offset = -1) {
    return async_read_awaitable(fd, buffer, length, offset);
}

/// Create an async read awaitable using span
template<typename T>
inline auto async_read(int fd, std::span<T> buffer, 
                       int64_t offset = -1) {
    return async_read_awaitable(fd, buffer.data(), 
                                buffer.size_bytes(), offset);
}

/// Create an async write awaitable
inline auto async_write(int fd, const void* buffer, 
                        size_t length, int64_t offset = -1) {
    return async_write_awaitable(fd, buffer, length, offset);
}

/// Create an async write awaitable using span
template<typename T>
inline auto async_write(int fd, std::span<const T> buffer,
                        int64_t offset = -1) {
    return async_write_awaitable(fd, buffer.data(), 
                                 buffer.size_bytes(), offset);
}

/// Create an async recv awaitable
inline auto async_recv(int fd, void* buffer, size_t length,
                       int flags = 0) {
    return async_recv_awaitable(fd, buffer, length, flags);
}

/// Create an async send awaitable
inline auto async_send(int fd, const void* buffer, 
                       size_t length, int flags = 0) {
    return async_send_awaitable(fd, buffer, length, flags);
}

/// Create an async readv awaitable (scatter-gather read)
inline auto async_readv(int fd, struct iovec* iovecs,
                        size_t iovec_count) {
    return async_readv_awaitable(fd, iovecs, iovec_count);
}

/// Create an async writev awaitable (scatter-gather write)
inline auto async_writev(int fd, struct iovec* iovecs,
                         size_t iovec_count) {
    return async_writev_awaitable(fd, iovecs, iovec_count);
}

/// Create an async accept awaitable
inline auto async_accept(int listen_fd,
                         struct sockaddr* addr = nullptr,
                         socklen_t* addrlen = nullptr,
                         int flags = 0) {
    return async_accept_awaitable(listen_fd, addr, addrlen, flags);
}

/// Create an async connect awaitable
inline auto async_connect(int fd, 
                          const struct sockaddr* addr, socklen_t addrlen) {
    return async_connect_awaitable(fd, addr, addrlen);
}

/// Create an async close awaitable
inline auto async_close(int fd) {
    return async_close_awaitable(fd);
}

/// Close an fd from a non-coroutine context (e.g. a destructor) without
/// racing in-flight io_uring SQEs against fd-table reuse.
///
/// Strategy:
///   * If the caller is running on a worker thread that uses the io_uring
///     backend, submit ``IORING_OP_CLOSE`` fire-and-forget. The kernel orders
///     this op after any earlier-submitted SQE on the same fd, so no
///     concurrent recv/send/poll can ever observe a recycled fd.
///   * Otherwise (epoll backend, or no current worker thread), fall back to
///     synchronous ``::close``. Epoll is a userspace readiness model — there
///     is no in-kernel SQE-vs-fd-reuse race to worry about — and a non-worker
///     destruction site cannot have outstanding SQEs of its own.
///
/// Always succeeds at releasing the fd: the worst case is an SQ-exhausted
/// io_uring backend where we transparently degrade to ``::close``.
inline void close_fd_for_destructor(int fd) noexcept {
    if (fd < 0) {
        return;
    }
#if ELIO_HAS_IO_URING
    auto* worker = runtime::worker_thread::current();
    if (worker) {
        auto& ctx = worker->io_context();
        if (ctx.is_io_uring()) {
            auto* backend = static_cast<io_uring_backend*>(ctx.get_backend());
            if (backend && backend->submit_close_async(fd)) {
                return;
            }
        }
    }
#endif
    ::close(fd);
}

/// Create an async poll awaitable for reading
inline auto async_poll_read(int fd) {
    return async_poll_awaitable(fd, true);
}

/// Create an async poll awaitable for writing
inline auto async_poll_write(int fd) {
    return async_poll_awaitable(fd, false);
}

/// Segment descriptor for batch read operations
struct batch_read_segment {
    int64_t offset;   ///< File offset (-1 for current position → uses pread with offset 0)
    void* buffer;     ///< Destination buffer
    size_t length;    ///< Bytes to read
};

/// Segment descriptor for batch write operations
struct batch_write_segment {
    int64_t offset;      ///< File offset (-1 for current position → uses pwrite with offset 0)
    const void* buffer;  ///< Source data
    size_t length;       ///< Bytes to write
};

// ``batch_state`` and ``batch_completion`` are defined in
// ``io_uring_backend.hpp`` (already included transitively via io_context.hpp)
// so the io_uring backend can dispatch CQEs to per-segment trampolines
// without any locks or maps.

namespace detail {

inline void mark_batch_inline_completed(batch_state& st) noexcept {
    st.phase.store(batch_state::phase_completed, std::memory_order_release);
}

#if ELIO_HAS_IO_URING
/// Submit a batch via io_uring without blocking the worker.
/// Returns true on success (caller must NOT resume; the final CQE will
/// resume the awaiter via the trampolines). Returns false if the io_uring
/// backend is unavailable or every SQE allocation failed; the caller should
/// then either fall back to sequential I/O or resume immediately.
template<typename PrepFn>
inline bool submit_batch_io_uring(io_uring_backend* backend,
                                  batch_state& st,
                                  std::coroutine_handle<> awaiter,
                                  PrepFn&& prep_one) {
    struct io_uring* ring = backend->get_ring();
    if (!ring) {
        return false;
    }

    const int total = st.total;
    st.awaiter = awaiter;
    st.awaiter_vstack = coro::vthread_stack::current();
    st.trampolines.resize(total);

    size_t in_flight = 0;
    size_t failed = 0;
    for (int i = 0; i < total; ++i) {
        struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            st.results[i] = -EAGAIN;
            ++failed;
            continue;
        }
        st.trampolines[i] = batch_completion{&st, static_cast<uint32_t>(i)};
        io_uring_sqe_set_data(sqe, st.trampolines[i].tagged_user_data());
        prep_one(sqe, i);
        ++in_flight;
    }

    if (in_flight == 0) {
        // Nothing made it onto the ring: no CQEs will fire, so the caller
        // must resume the awaiter inline. Reset awaiter so a stray CQE
        // (there should be none) cannot resume twice.
        st.awaiter = {};
        st.awaiter_vstack = nullptr;
        st.completed.store(total, std::memory_order_release);
        return false;
    }

    // Account for the failed SQEs first so the in-flight count is exactly
    // what's needed to drive ``completed`` to ``total``.
    if (failed) {
        st.completed.fetch_add(static_cast<int>(failed), std::memory_order_acq_rel);
    }

    backend->register_pending(in_flight);
    int submitted = io_uring_submit(ring);
    if (submitted < 0) {
        // Submit failed entirely: rollback pending_ops_ and mark all
        // in-flight segments as failed so the awaiter resumes promptly.
        ELIO_LOG_ERROR("submit_batch_io_uring: io_uring_submit failed: {}",
                       strerror(-submitted));
        backend->unregister_pending(in_flight);
        st.completed.fetch_add(static_cast<int>(in_flight), std::memory_order_acq_rel);
        for (size_t i = 0; i < static_cast<size_t>(st.total); ++i) {
            if (st.results[i] == 0) {
                st.results[i] = submitted;  // propagate the error
            }
        }
        return false;
    }
    if (static_cast<size_t>(submitted) < in_flight) {
        // Partial submit: rollback the excess and mark un-submitted segments.
        size_t excess = in_flight - static_cast<size_t>(submitted);
        backend->unregister_pending(excess);
        st.completed.fetch_add(static_cast<int>(excess), std::memory_order_acq_rel);
        ELIO_LOG_WARNING("submit_batch_io_uring: submitted {}/{} SQEs",
                         submitted, in_flight);
    }
    return true;
}
#endif

} // namespace detail

/// Awaitable for batch read operations
/// Submits multiple pread operations in a single io_uring syscall.
/// Falls back to sequential synchronous reads for epoll backend.
class batch_read_awaitable : public io_awaitable_base {
public:
    batch_read_awaitable(int fd, std::span<const batch_read_segment> segments) noexcept
        : io_awaitable_base(), fd_(fd), segments_(segments.begin(), segments.end()) {}

    ~batch_read_awaitable() {
        if (!batch_st_) return;
        // Orphan protocol: if SQEs are still in flight when the awaitable
        // is destroyed (forced cancel, vthread teardown), CAS phase to
        // orphaned so dispatch_batch_completion frees the state instead
        // of resuming a dangling awaiter.
        uint8_t expected = batch_state::phase_pending;
        if (batch_st_->phase.compare_exchange_strong(
                expected, batch_state::phase_orphaned,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            // We won the CAS: CQE handler will delete. Release ownership.
            (void)batch_st_.release();
        }
        // If CAS failed, phase was already completed or something else. A
        // completed final CQE has already copied everything it still needs,
        // so unique_ptr cleans up normally.
    }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);

        if (segments_.empty()) {
            // No segments, no need to allocate batch_state
            awaiter.resume();
            return;
        }

#if ELIO_HAS_IO_URING
        auto& ctx = current_io_context();
        // Fast path: skip RTTI/dynamic_cast — io_context already exposes
        // an is_io_uring() check, and we know the backend hierarchy
        // statically here.
        auto* backend = ctx.is_io_uring()
            ? static_cast<io_uring_backend*>(ctx.get_backend())
            : nullptr;
        if (backend && backend->get_ring()) {
            batch_st_ = std::make_unique<batch_state>(static_cast<int>(segments_.size()));
            const int n = batch_st_->total;
            bool submitted = detail::submit_batch_io_uring(
                backend, *batch_st_, awaiter,
                [this, n](struct io_uring_sqe* sqe, int i) {
                    auto off = static_cast<__u64>(
                        segments_[i].offset >= 0 ? segments_[i].offset : 0);
                    io_uring_prep_read(sqe, fd_, segments_[i].buffer,
                        static_cast<unsigned>(segments_[i].length), off);
                    (void)n;
                });
            if (!submitted) {
                // No SQEs made it onto the ring: every result is already
                // -EAGAIN. Resume inline.
                detail::mark_batch_inline_completed(*batch_st_);
                awaiter.resume();
            }
            // Otherwise the final CQE drives the resume via the backend.
            return;
        }
#endif
        // Fallback: sequential synchronous reads (no io_uring available).
        batch_st_ = std::make_unique<batch_state>(static_cast<int>(segments_.size()));
        for (size_t i = 0; i < segments_.size(); ++i) {
            auto off = segments_[i].offset >= 0 ? segments_[i].offset : 0;
            auto result = pread(fd_, segments_[i].buffer, segments_[i].length, off);
            int error = errno;
            batch_st_->results[i] = result < 0 ? -error : static_cast<int>(result);
        }
        detail::mark_batch_inline_completed(*batch_st_);
        awaiter.resume();
    }

    std::vector<int> await_resume() noexcept {
        restore_affinity();
        if (batch_st_) {
            return std::move(batch_st_->results);
        }
        return {};
    }

private:
    int fd_;
    std::vector<batch_read_segment> segments_;
    std::unique_ptr<batch_state> batch_st_;
};

/// Awaitable for batch write operations
/// Submits multiple pwrite operations in a single io_uring syscall.
/// Falls back to sequential synchronous writes for epoll backend.
class batch_write_awaitable : public io_awaitable_base {
public:
    batch_write_awaitable(int fd, std::span<const batch_write_segment> segments) noexcept
        : io_awaitable_base(), fd_(fd), segments_(segments.begin(), segments.end()) {}

    ~batch_write_awaitable() {
        if (!batch_st_) return;
        uint8_t expected = batch_state::phase_pending;
        if (batch_st_->phase.compare_exchange_strong(
                expected, batch_state::phase_orphaned,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            (void)batch_st_.release();
        }
    }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        bind_to_worker(awaiter);

        if (segments_.empty()) {
            // No segments, no need to allocate batch_state
            awaiter.resume();
            return;
        }

#if ELIO_HAS_IO_URING
        auto& ctx = current_io_context();
        // Fast path: skip RTTI/dynamic_cast — io_context already exposes
        // an is_io_uring() check, and we know the backend hierarchy
        // statically here.
        auto* backend = ctx.is_io_uring()
            ? static_cast<io_uring_backend*>(ctx.get_backend())
            : nullptr;
        if (backend && backend->get_ring()) {
            batch_st_ = std::make_unique<batch_state>(static_cast<int>(segments_.size()));
            bool submitted = detail::submit_batch_io_uring(
                backend, *batch_st_, awaiter,
                [this](struct io_uring_sqe* sqe, int i) {
                    auto off = static_cast<__u64>(
                        segments_[i].offset >= 0 ? segments_[i].offset : 0);
                    io_uring_prep_write(sqe, fd_, segments_[i].buffer,
                        static_cast<unsigned>(segments_[i].length), off);
                });
            if (!submitted) {
                detail::mark_batch_inline_completed(*batch_st_);
                awaiter.resume();
            }
            return;
        }
#endif
        // Fallback: sequential synchronous writes.
        batch_st_ = std::make_unique<batch_state>(static_cast<int>(segments_.size()));
        for (size_t i = 0; i < segments_.size(); ++i) {
            auto off = segments_[i].offset >= 0 ? segments_[i].offset : 0;
            auto result = pwrite(fd_, segments_[i].buffer, segments_[i].length, off);
            int error = errno;
            batch_st_->results[i] = result < 0 ? -error : static_cast<int>(result);
        }
        detail::mark_batch_inline_completed(*batch_st_);
        awaiter.resume();
    }

    std::vector<int> await_resume() noexcept {
        restore_affinity();
        if (batch_st_) {
            return std::move(batch_st_->results);
        }
        return {};
    }

private:
    int fd_;
    std::vector<batch_write_segment> segments_;
    std::unique_ptr<batch_state> batch_st_;
};

/// Batch read from file at multiple offsets in a single syscall (io_uring)
inline auto batch_read(int fd, std::span<const batch_read_segment> segments) {
    return batch_read_awaitable(fd, segments);
}

/// Batch write to file at multiple offsets in a single syscall (io_uring)
inline auto batch_write(int fd, std::span<const batch_write_segment> segments) {
    return batch_write_awaitable(fd, segments);
}

// ============================================================================
// Cancellable I/O awaitables
// ============================================================================

namespace detail {

/// Shared state for cancellable I/O operations.
/// Follows the same pattern as cancellable_sleep_awaitable::shared_state.
struct io_cancel_state {
    io_context* ctx = nullptr;
    std::coroutine_handle<> awaiter;
    runtime::worker_thread* worker = nullptr;
    op_state* op = nullptr;
    std::atomic<bool> resumed{false};
    std::atomic<bool> cancelled{false};
};

/// Fire-and-forget coroutine that executes io_context::cancel() on the worker
/// that owns the ring. Self-destroys via suspend_never on final_suspend.
///
/// NOTE: promise_type MUST inherit coro::promise_base to enable the affinity
/// mechanism. Without it, try_steal() sees NO_AFFINITY and allows stealing
/// this task to other workers, which then access the wrong io_context's
/// io_uring ring — causing data races.
struct io_cancel_executor {
    struct promise_type : public coro::promise_base {
        io_cancel_executor get_return_object() {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
    };
    std::coroutine_handle<promise_type> handle;
};

inline io_cancel_executor make_io_cancel_executor(
    std::shared_ptr<io_cancel_state> state,
    bool allow_epoll_cancel = false) {
    if (state->resumed.exchange(true, std::memory_order_acq_rel)) {
        co_return;
    }
    if (state->ctx && state->op) {
#if ELIO_HAS_IO_URING
        if (state->ctx->is_io_uring()) {
            state->ctx->cancel(tagged_op_state_user_data(state->op));
        } else if (allow_epoll_cancel) {
            state->ctx->cancel(tagged_op_state_user_data(state->op));
        } else {
            ELIO_LOG_WARNING(
                "cancellable I/O: epoll backend does not support async cancel "
                "(fd reuse race risk); cancel_token is no-op");
        }
#else
        if (allow_epoll_cancel) {
            state->ctx->cancel(tagged_op_state_user_data(state->op));
        } else {
            ELIO_LOG_WARNING(
                "cancellable I/O: epoll backend does not support async cancel "
                "(fd reuse race risk); cancel_token is no-op");
        }
#endif
    }
    co_return;
}

inline io_result finalize_cancellable_io_result(
    bool cancelled_without_backend_completion,
    io_result result) noexcept {
    if (cancelled_without_backend_completion && result.success()) {
        return io_result{-ECANCELED, result.flags};
    }
    return result;
}

inline coro::cancel_result classify_cancellable_io_result(
    bool cancelled_without_backend_completion,
    io_result result) noexcept {
    return (cancelled_without_backend_completion || result.result == -ECANCELED)
        ? coro::cancel_result::cancelled
        : coro::cancel_result::completed;
}

}  // namespace detail

/// Result of a cancellable I/O operation
struct cancellable_io_result {
    io_result io;
    coro::cancel_result cancel;

    bool success() const noexcept { return io.success(); }
    bool was_cancelled() const noexcept {
        return cancel == coro::cancel_result::cancelled;
    }
    int bytes_transferred() const noexcept { return io.bytes_transferred(); }
    int error_code() const noexcept { return io.error_code(); }
};

/// Awaitable for cancellable async recv operations
class cancellable_async_recv_awaitable : public io_awaitable_base {
public:
    cancellable_async_recv_awaitable(int fd, void* buffer, size_t length,
                                      int flags, coro::cancel_token token) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , buffer_(buffer)
        , length_(length)
        , flags_(flags)
        , token_(std::move(token)) {}

    bool await_ready() const noexcept {
        return token_.is_cancelled();
    }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        if (token_.is_cancelled()) {
            already_cancelled_before_setup_ = true;
            result_ = io_result{-ECANCELED, 0};
            awaiter.resume();
            return;
        }

        bind_to_worker(awaiter);
        auto& ctx = current_io_context();

        // Allocate shared state for cancel callback
        auto state = std::make_shared<detail::io_cancel_state>();
        state->ctx = &ctx;
        state->awaiter = awaiter;
        state->worker = runtime::worker_thread::current();
        state_ = state;

        // Register cancel callback
        cancel_registration_ = token_.on_cancel([state]() {
            state->cancelled.store(true, std::memory_order_release);
            if (!state->worker) {
                return;
            }
            auto exec = detail::make_io_cancel_executor(
                state, /*allow_epoll_cancel=*/true);
            if (auto* promise = coro::get_promise_base(exec.handle.address())) {
                promise->set_affinity(state->worker->worker_id());
                promise->set_worker_local();
                promise->detach_from_parent();
            }
            state->worker->schedule_or_destroy(exec.handle);
        });

        // Check again after registration
        if (token_.is_cancelled()) {
            cancel_registration_.unregister();
            state->resumed.store(true, std::memory_order_release);
            result_ = io_result{-ECANCELED, 0};
            awaiter.resume();
            return;
        }

        io_request req{};
        req.op = io_op::recv;
        req.fd = fd_;
        req.buffer = buffer_;
        req.length = length_;
        req.socket_flags = flags_;
        req.awaiter = awaiter;
        req.state = setup_op_state(awaiter);
        state->op = req.state;

        if (!ctx.prepare(req)) {
            clear_op_state();
            state->op = nullptr;
            cancel_registration_.unregister();
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }

        // Post-registration race: cancel may have fired between on_cancel()
        // and setting state->op above. Re-check after prepare so the backend
        // can actually find the staged operation and abort it.
        if (state->cancelled.load(std::memory_order_acquire)) {
            ctx.cancel(tagged_op_state_user_data(req.state));
        }
    }

    cancellable_io_result await_resume() noexcept {
        cancel_registration_.unregister();
        const bool cancelled_without_backend_completion =
            already_cancelled_before_setup_ ||
            (!state_ && token_.is_cancelled());
        if (state_) {
            state_->resumed.store(true, std::memory_order_release);
        }
        result_ = detail::finalize_cancellable_io_result(
            cancelled_without_backend_completion, read_result_from_op_state());
        restore_affinity();
        return cancellable_io_result{
            result_,
            detail::classify_cancellable_io_result(
                cancelled_without_backend_completion, result_)
        };
    }

private:
    int fd_;
    void* buffer_;
    size_t length_;
    int flags_;
    coro::cancel_token token_;
    coro::cancel_token::registration cancel_registration_;
    std::shared_ptr<detail::io_cancel_state> state_;
    bool already_cancelled_before_setup_ = false;
};

/// Awaitable for cancellable async send operations
class cancellable_async_send_awaitable : public io_awaitable_base {
public:
    cancellable_async_send_awaitable(int fd, const void* buffer,
                                      size_t length, int flags,
                                      coro::cancel_token token) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , buffer_(buffer)
        , length_(length)
        , flags_(flags)
        , token_(std::move(token)) {}

    bool await_ready() const noexcept {
        return token_.is_cancelled();
    }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        if (token_.is_cancelled()) {
            already_cancelled_before_setup_ = true;
            result_ = io_result{-ECANCELED, 0};
            awaiter.resume();
            return;
        }

        bind_to_worker(awaiter);
        auto& ctx = current_io_context();

        auto state = std::make_shared<detail::io_cancel_state>();
        state->ctx = &ctx;
        state->awaiter = awaiter;
        state->worker = runtime::worker_thread::current();
        state_ = state;

        cancel_registration_ = token_.on_cancel([state]() {
            state->cancelled.store(true, std::memory_order_release);
            if (!state->worker) {
                return;
            }
            auto exec = detail::make_io_cancel_executor(
                state, /*allow_epoll_cancel=*/true);
            if (auto* promise = coro::get_promise_base(exec.handle.address())) {
                promise->set_affinity(state->worker->worker_id());
                promise->set_worker_local();
                promise->detach_from_parent();
            }
            state->worker->schedule_or_destroy(exec.handle);
        });

        if (token_.is_cancelled()) {
            cancel_registration_.unregister();
            state->resumed.store(true, std::memory_order_release);
            result_ = io_result{-ECANCELED, 0};
            awaiter.resume();
            return;
        }

        io_request req{};
        req.op = io_op::send;
        req.fd = fd_;
        req.buffer = const_cast<void*>(buffer_);
        req.length = length_;
        req.socket_flags = flags_;
        req.awaiter = awaiter;
        req.state = setup_op_state(awaiter);
        state->op = req.state;

        if (!ctx.prepare(req)) {
            clear_op_state();
            state->op = nullptr;
            cancel_registration_.unregister();
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }

        // Post-registration race: cancel may have fired between on_cancel()
        // and setting state->op above. Re-check after prepare so the backend
        // can actually find the staged operation and abort it.
        if (state->cancelled.load(std::memory_order_acquire)) {
            ctx.cancel(tagged_op_state_user_data(req.state));
        }
    }

    cancellable_io_result await_resume() noexcept {
        cancel_registration_.unregister();
        const bool cancelled_without_backend_completion =
            already_cancelled_before_setup_ ||
            (!state_ && token_.is_cancelled());
        if (state_) {
            state_->resumed.store(true, std::memory_order_release);
        }
        result_ = detail::finalize_cancellable_io_result(
            cancelled_without_backend_completion, read_result_from_op_state());
        restore_affinity();
        return cancellable_io_result{
            result_,
            detail::classify_cancellable_io_result(
                cancelled_without_backend_completion, result_)
        };
    }

private:
    int fd_;
    const void* buffer_;
    size_t length_;
    int flags_;
    coro::cancel_token token_;
    coro::cancel_token::registration cancel_registration_;
    std::shared_ptr<detail::io_cancel_state> state_;
    bool already_cancelled_before_setup_ = false;
};

/// Awaitable for cancellable async connect operations
class cancellable_async_connect_awaitable : public io_awaitable_base {
public:
    cancellable_async_connect_awaitable(int fd,
                                         const struct sockaddr* addr,
                                         socklen_t addrlen,
                                         coro::cancel_token token) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , addr_(addr)
        , addrlen_(addrlen)
        , token_(std::move(token)) {}

    bool await_ready() const noexcept {
        return token_.is_cancelled();
    }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        if (token_.is_cancelled()) {
            already_cancelled_before_setup_ = true;
            result_ = io_result{-ECANCELED, 0};
            awaiter.resume();
            return;
        }

        bind_to_worker(awaiter);
        auto& ctx = current_io_context();

        auto state = std::make_shared<detail::io_cancel_state>();
        state->ctx = &ctx;
        state->awaiter = awaiter;
        state->worker = runtime::worker_thread::current();
        state_ = state;

        cancel_registration_ = token_.on_cancel([state]() {
            state->cancelled.store(true, std::memory_order_release);
            if (!state->worker) {
                return;
            }
            auto exec = detail::make_io_cancel_executor(
                state, /*allow_epoll_cancel=*/true);
            if (auto* promise = coro::get_promise_base(exec.handle.address())) {
                promise->set_affinity(state->worker->worker_id());
                promise->set_worker_local();
                promise->detach_from_parent();
            }
            state->worker->schedule_or_destroy(exec.handle);
        });

        if (token_.is_cancelled()) {
            cancel_registration_.unregister();
            state->resumed.store(true, std::memory_order_release);
            result_ = io_result{-ECANCELED, 0};
            awaiter.resume();
            return;
        }

        io_request req{};
        req.op = io_op::connect;
        req.fd = fd_;
        req.addr = const_cast<struct sockaddr*>(addr_);
        req.addrlen = &addrlen_;
        req.awaiter = awaiter;
        req.state = setup_op_state(awaiter);
        state->op = req.state;

        if (!ctx.prepare(req)) {
            clear_op_state();
            state->op = nullptr;
            cancel_registration_.unregister();
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }

        // Post-registration race: cancel may have fired between on_cancel()
        // and setting state->op above. Re-check after prepare so the backend
        // can actually find the staged operation and abort it.
        if (state->cancelled.load(std::memory_order_acquire)) {
            ctx.cancel(tagged_op_state_user_data(req.state));
        }
    }

    cancellable_io_result await_resume() noexcept {
        cancel_registration_.unregister();
        const bool cancelled_without_backend_completion =
            already_cancelled_before_setup_ ||
            (!state_ && token_.is_cancelled());
        if (state_) {
            state_->resumed.store(true, std::memory_order_release);
        }
        result_ = detail::finalize_cancellable_io_result(
            cancelled_without_backend_completion, read_result_from_op_state());
        restore_affinity();
        return cancellable_io_result{
            result_,
            detail::classify_cancellable_io_result(
                cancelled_without_backend_completion, result_)
        };
    }

private:
    int fd_;
    const struct sockaddr* addr_;
    socklen_t addrlen_;
    coro::cancel_token token_;
    coro::cancel_token::registration cancel_registration_;
    std::shared_ptr<detail::io_cancel_state> state_;
    bool already_cancelled_before_setup_ = false;
};

/// Awaitable for cancellable async poll operations
class cancellable_async_poll_awaitable : public io_awaitable_base {
public:
    cancellable_async_poll_awaitable(int fd, bool for_read,
                                     coro::cancel_token token) noexcept
        : io_awaitable_base()
        , fd_(fd)
        , for_read_(for_read)
        , token_(std::move(token)) {}

    bool await_ready() const noexcept {
        return token_.is_cancelled();
    }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiter) {
        if (token_.is_cancelled()) {
            already_cancelled_before_setup_ = true;
            result_ = io_result{-ECANCELED, 0};
            awaiter.resume();
            return;
        }

        bind_to_worker(awaiter);
        auto& ctx = current_io_context();

        auto state = std::make_shared<detail::io_cancel_state>();
        state->ctx = &ctx;
        state->awaiter = awaiter;
        state->worker = runtime::worker_thread::current();
        state_ = state;

        cancel_registration_ = token_.on_cancel([state]() {
            state->cancelled.store(true, std::memory_order_release);
            if (!state->worker) {
                return;
            }
            auto exec = detail::make_io_cancel_executor(
                state, /*allow_epoll_cancel=*/true);
            if (auto* promise = coro::get_promise_base(exec.handle.address())) {
                promise->set_affinity(state->worker->worker_id());
                promise->set_worker_local();
                promise->detach_from_parent();
            }
            state->worker->schedule_or_destroy(exec.handle);
        });

        if (token_.is_cancelled()) {
            cancel_registration_.unregister();
            state->resumed.store(true, std::memory_order_release);
            result_ = io_result{-ECANCELED, 0};
            awaiter.resume();
            return;
        }

        io_request req{};
        req.op = for_read_ ? io_op::poll_read : io_op::poll_write;
        req.fd = fd_;
        req.awaiter = awaiter;
        req.state = setup_op_state(awaiter);
        state->op = req.state;

        if (!ctx.prepare(req)) {
            clear_op_state();
            state->op = nullptr;
            cancel_registration_.unregister();
            result_ = io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }

        // Post-registration race: cancel may have fired between on_cancel()
        // and setting state->op above. Re-check and submit async cancel
        // inline so the poll op is actually aborted.
        if (state->cancelled.load(std::memory_order_acquire)) {
            ctx.cancel(tagged_op_state_user_data(req.state));
        }
    }

    cancellable_io_result await_resume() noexcept {
        cancel_registration_.unregister();
        const bool cancelled_without_backend_completion =
            already_cancelled_before_setup_ ||
            (!state_ && token_.is_cancelled());
        if (state_) {
            state_->resumed.store(true, std::memory_order_release);
        }
        result_ = detail::finalize_cancellable_io_result(
            cancelled_without_backend_completion, read_result_from_op_state());
        restore_affinity();
        return cancellable_io_result{
            result_,
            detail::classify_cancellable_io_result(
                cancelled_without_backend_completion, result_)
        };
    }

private:
    int fd_;
    bool for_read_;
    coro::cancel_token token_;
    coro::cancel_token::registration cancel_registration_;
    std::shared_ptr<detail::io_cancel_state> state_;
    bool already_cancelled_before_setup_ = false;
};

/// Awaitable for cancellable async poll-read operations
class cancellable_async_poll_read_awaitable
    : public cancellable_async_poll_awaitable {
public:
    cancellable_async_poll_read_awaitable(int fd,
                                          coro::cancel_token token) noexcept
        : cancellable_async_poll_awaitable(fd, true, std::move(token)) {}
};

/// Awaitable for cancellable async poll-write operations
class cancellable_async_poll_write_awaitable
    : public cancellable_async_poll_awaitable {
public:
    cancellable_async_poll_write_awaitable(int fd,
                                           coro::cancel_token token) noexcept
        : cancellable_async_poll_awaitable(fd, false, std::move(token)) {}
};

/// Factory functions for cancellable I/O operations

/// Create a cancellable async recv awaitable
inline auto async_recv(int fd, void* buffer, size_t length,
                       int flags, coro::cancel_token token) {
    return cancellable_async_recv_awaitable(fd, buffer, length, flags,
                                             std::move(token));
}

/// Create a cancellable async send awaitable
inline auto async_send(int fd, const void* buffer,
                       size_t length, int flags, coro::cancel_token token) {
    return cancellable_async_send_awaitable(fd, buffer, length, flags,
                                             std::move(token));
}

/// Create a cancellable async connect awaitable
inline auto async_connect(int fd,
                          const struct sockaddr* addr, socklen_t addrlen,
                          coro::cancel_token token) {
    return cancellable_async_connect_awaitable(fd, addr, addrlen,
                                                std::move(token));
}

/// Create a cancellable async poll awaitable for reading
inline auto async_poll_read(int fd, coro::cancel_token token) {
    return cancellable_async_poll_read_awaitable(fd, std::move(token));
}

/// Create a cancellable async poll awaitable for writing
inline auto async_poll_write(int fd, coro::cancel_token token) {
    return cancellable_async_poll_write_awaitable(fd, std::move(token));
}

} // namespace elio::io
