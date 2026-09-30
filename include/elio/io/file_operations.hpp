#pragma once

#include "io_awaitables.hpp"
#include <elio/coro/task.hpp>
#include <elio/runtime/spawn_blocking.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

namespace elio::io {

enum class file_sync_mode { data_only, data_and_metadata };
enum class file_operation_end { complete, error, cancelled };

struct file_status {
    file_operation_end end = file_operation_end::complete;
    std::error_code error;

    explicit operator bool() const noexcept { return end == file_operation_end::complete; }
    int error_value() const noexcept { return error.value(); }
};

struct file_operation_options {
    /// Maximum queued items in the shared scheduler blocking pool at admission.
    /// Running calls are separately bounded by the fixed pool workers.
    size_t max_queued = 256;
};

namespace detail {

inline constexpr uint64_t file_operation_max_offset = std::min(
    static_cast<uint64_t>(std::numeric_limits<int64_t>::max()),
    static_cast<uint64_t>(std::numeric_limits<off_t>::max()));

inline file_status file_operation_failure(
        int error, file_operation_end end = file_operation_end::error) noexcept {
    return {end, std::error_code(error, std::generic_category())};
}

struct file_operation_request {
    io_op operation;
    int fd;
    int flags = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
};

#ifdef ELIO_RUNTIME_TEST_HOOKS
using file_syscall_hook = int (*)(const file_operation_request&);
inline std::atomic<file_syscall_hook> file_syscall_for_test{nullptr};
inline std::atomic<bool> force_file_operations_fallback_for_test{false};
#endif

inline file_status execute_file_syscall(const file_operation_request& request) noexcept {
    int result = -1;
#ifdef ELIO_RUNTIME_TEST_HOOKS
    if (auto hook = file_syscall_for_test.load(std::memory_order_acquire)) {
        result = hook(request);
    } else
#endif
    {
        switch (request.operation) {
            case io_op::file_sync:
                result = request.flags != 0 ? ::fdatasync(request.fd) : ::fsync(request.fd);
                break;
            case io_op::file_allocate:
                result = ::fallocate(request.fd, request.flags,
                    static_cast<off_t>(request.offset), static_cast<off_t>(request.length));
                break;
            case io_op::file_truncate:
                result = ::ftruncate(request.fd, static_cast<off_t>(request.length));
                break;
            default:
                return file_operation_failure(EINVAL);
        }
    }
    if (result == 0) return {};
    return file_operation_failure(result < 0 ? errno : EIO);
}

class native_file_operation : public io_awaitable_base {
public:
    explicit native_file_operation(file_operation_request request) noexcept
        : request_(request) {}

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> awaiter) {
        auto& context = current_io_context();
        io_request request{};
        request.op = request_.operation;
        request.fd = request_.fd;
        request.file_flags = request_.flags;
        request.offset = static_cast<int64_t>(request_.operation == io_op::file_truncate
            ? request_.length : request_.offset);
        request.file_length = request_.length;
        request.awaiter = awaiter;
        request.state = setup_op_state(awaiter, context);
        if (!prepare_op_state(context, request)) {
            clear_op_state();
            result_ = prepare_failure_result();
            return false;
        }
        return true;
    }

    file_status await_resume() noexcept {
        result_ = read_result_from_op_state();
        if (result_.result == 0) return {};
        const int error = result_.result < 0 &&
                result_.result != std::numeric_limits<int32_t>::min()
            ? -result_.result : EIO;
        return file_operation_failure(error);
    }

private:
    file_operation_request request_;
};

inline coro::task<file_status> dispatch_file_operation(
        file_operation_request request, coro::cancel_token token,
        file_operation_options options) {
    if (options.max_queued == 0 ||
            options.max_queued == std::numeric_limits<size_t>::max()) {
        co_return file_operation_failure(EINVAL);
    }
    if (request.offset > file_operation_max_offset ||
            request.length > file_operation_max_offset - request.offset) {
        co_return file_operation_failure(EOVERFLOW);
    }
    if (request.operation == io_op::file_allocate && request.length == 0) {
        co_return file_operation_failure(EINVAL);
    }
    if (token.is_cancelled()) {
        co_return file_operation_failure(ECANCELED, file_operation_end::cancelled);
    }
    auto* scheduler = runtime::get_current_scheduler();
    if (!runtime::worker_thread::current() || !scheduler || !scheduler->is_running()) {
        co_return file_operation_failure(ENOTSUP);
    }

    bool native = current_io_context().supports_file_operation(request.operation);
#ifdef ELIO_RUNTIME_TEST_HOOKS
    native = native && !force_file_operations_fallback_for_test.load(std::memory_order_acquire);
#endif
    if (native) {
        // Never replay a terminal native error through the blocking fallback.
        co_return co_await native_file_operation(request);
    }

    auto work = [request, token = std::move(token)]() noexcept -> file_status {
        if (token.is_cancelled()) {
            return file_operation_failure(ECANCELED, file_operation_end::cancelled);
        }
        // Pool shutdown can drain queued work on its caller. Reject that
        // dispatch if a scheduler worker initiated teardown; never run a
        // potentially blocking file syscall on the scheduler worker itself.
        if (runtime::worker_thread::current()) {
            return file_operation_failure(EAGAIN);
        }
        // This check is the dispatch boundary; later cancellation does not
        // interrupt the syscall or overwrite its actual terminal result.
        return execute_file_syscall(request);
    };
    try {
        co_return co_await elio::detail::blocking_awaitable<file_status, decltype(work)>(
            std::move(work), options.max_queued);
    } catch (const elio::detail::blocking_admission_error&) {
        co_return file_operation_failure(EAGAIN);
    }
}

} // namespace detail

/// FD-based operations require a running scheduler worker. Native capabilities are
/// probed; unsupported native paths use bounded fixed-pool admission, never
/// inline worker syscalls or detached threads. Keep the borrowed FD open and
/// unrecycled until normal awaited return. Cancellation may skip queued work,
/// but admitted native/running blocking work completes with its actual result.
/// No deadline, rollback, transaction, or forced-teardown barrier is supplied.
inline coro::task<file_status> sync_file(
        int fd, file_sync_mode mode = file_sync_mode::data_and_metadata,
        coro::cancel_token token = {}, file_operation_options options = {}) {
    if (mode != file_sync_mode::data_only && mode != file_sync_mode::data_and_metadata) {
        co_return detail::file_operation_failure(EINVAL);
    }
    co_return co_await detail::dispatch_file_operation(
        {io_op::file_sync, fd, mode == file_sync_mode::data_only ? 1 : 0},
        std::move(token), options);
}

inline coro::task<file_status> allocate_file_range(
        int fd, int flags, uint64_t offset, uint64_t length,
        coro::cancel_token token = {}, file_operation_options options = {}) {
    return detail::dispatch_file_operation(
        {io_op::file_allocate, fd, flags, offset, length}, std::move(token), options);
}

inline coro::task<file_status> truncate_file(
        int fd, uint64_t length, coro::cancel_token token = {},
        file_operation_options options = {}) {
    return detail::dispatch_file_operation(
        {io_op::file_truncate, fd, 0, 0, length}, std::move(token), options);
}

} // namespace elio::io
