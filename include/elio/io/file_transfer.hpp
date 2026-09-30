#pragma once

#include "io_awaitables.hpp"
#include <elio/coro/task.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <system_error>
#include <sys/types.h>
#include <utility>

namespace elio::io {

enum class transfer_end { complete, eof, error, cancelled };

struct file_transfer_result {
    size_t transferred = 0;
    transfer_end end = transfer_end::complete;
    std::error_code error;
};

namespace detail {

inline constexpr size_t file_transfer_max_chunk =
    static_cast<size_t>(std::numeric_limits<int32_t>::max());
inline constexpr uint64_t file_transfer_max_offset = std::min(
    static_cast<uint64_t>(std::numeric_limits<int64_t>::max()),
    static_cast<uint64_t>(std::numeric_limits<off_t>::max()));

inline file_transfer_result file_transfer_failure(
        size_t transferred, int error, transfer_end end = transfer_end::error) noexcept {
    return {transferred, end, std::error_code(error, std::generic_category())};
}

template<bool Write, typename Byte, typename Operation>
coro::task<file_transfer_result> transfer_file_range(
        int fd, std::span<Byte> buffer, uint64_t offset, bool exact,
        coro::cancel_token token, Operation operation) {
    // Validate the entire requested range before a write can have side effects.
    if (offset > file_transfer_max_offset ||
            buffer.size() > file_transfer_max_offset - offset) {
        co_return file_transfer_failure(0, EOVERFLOW);
    }

    file_transfer_result result;
    while (result.transferred < buffer.size()) {
        if (token.is_cancelled()) {
            co_return file_transfer_failure(
                result.transferred, ECANCELED, transfer_end::cancelled);
        }
        const auto count = std::min(
            buffer.size() - result.transferred, file_transfer_max_chunk);
        const auto completion = co_await operation(
            fd, buffer.subspan(result.transferred, count),
            static_cast<int64_t>(offset + result.transferred));
        if (completion.result < 0) {
            if (completion.result == -EINTR) {
                continue;
            }
            // Backend errors are -errno. Avoid signed overflow even if an
            // adapter supplies a malformed result outside that contract.
            const int error = completion.result == std::numeric_limits<int32_t>::min()
                ? EIO : -completion.result;
            co_return file_transfer_failure(result.transferred, error,
                error == ECANCELED ? transfer_end::cancelled : transfer_end::error);
        }
        if (completion.result == 0) {
            if constexpr (Write) {
                co_return file_transfer_failure(result.transferred, EIO);
            } else {
                result.end = transfer_end::eof;
                co_return result;
            }
        }
        const auto completed = static_cast<size_t>(completion.result);
        if (completed > count) {
            co_return file_transfer_failure(result.transferred, EIO);
        }
        result.transferred += completed;
        if (!exact) {
            co_return result;
        }
    }
    co_return result;
}

struct positional_read {
    auto operator()(int fd, std::span<std::byte> buffer, int64_t offset) const noexcept {
        return async_read(fd, buffer.data(), buffer.size(), offset);
    }
};

struct positional_write {
    auto operator()(int fd, std::span<const std::byte> buffer, int64_t offset) const noexcept {
        return async_write(fd, buffer.data(), buffer.size(), offset);
    }
};

} // namespace detail

/// Positional regular-file transfers over borrowed buffers. Keep the FD open
/// and unrecycled and the buffer alive until return; writes require no O_APPEND.
/// Range validation precedes I/O. Empty spans complete without submission.
/// EINTR retries; EAGAIN returns an error without spinning. Token cancellation
/// stops only before the next operation, never abandons an in-flight operation,
/// and supplies no deadline. Existing backend execution policy is unchanged.
inline coro::task<file_transfer_result> pread_some(
        int fd, std::span<std::byte> out, uint64_t offset, coro::cancel_token token = {}) {
    return detail::transfer_file_range<false>(
        fd, out, offset, false, std::move(token), detail::positional_read{});
}

inline coro::task<file_transfer_result> pwrite_some(
        int fd, std::span<const std::byte> in, uint64_t offset, coro::cancel_token token = {}) {
    return detail::transfer_file_range<true>(
        fd, in, offset, false, std::move(token), detail::positional_write{});
}

/// Exact success transfers the whole span. EOF, later errors, or between-step
/// cancellation retain prior progress; partial writes are never rolled back.
inline coro::task<file_transfer_result> pread_exactly(
        int fd, std::span<std::byte> out, uint64_t offset, coro::cancel_token token = {}) {
    return detail::transfer_file_range<false>(
        fd, out, offset, true, std::move(token), detail::positional_read{});
}

inline coro::task<file_transfer_result> pwrite_exactly(
        int fd, std::span<const std::byte> in, uint64_t offset, coro::cancel_token token = {}) {
    return detail::transfer_file_range<true>(
        fd, in, offset, true, std::move(token), detail::positional_write{});
}

} // namespace elio::io
