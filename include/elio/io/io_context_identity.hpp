#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace elio::io::detail {

inline constexpr size_t NO_IO_CONTEXT_OWNER =
    std::numeric_limits<size_t>::max();

/// Stable ownership metadata shared by an io_context and its in-flight
/// operations. The shared lifetime lets orphaned operation state release its
/// accounting after the coroutine frame has already been destroyed.
struct io_context_identity final {
    io_context_identity(size_t owner_worker, uint64_t context_generation) noexcept
        : owner_worker_id(owner_worker)
        , generation(context_generation) {}

    [[nodiscard]] bool is_worker_owned() const noexcept {
        return owner_worker_id != NO_IO_CONTEXT_OWNER;
    }

    const size_t owner_worker_id;
    const uint64_t generation;
    std::atomic<size_t> active_pins{0};
    std::atomic<const void*> owner_thread_token{nullptr};
};

inline thread_local const io_context_identity* current_worker_io_context =
    nullptr;

inline const void* current_io_thread_token() noexcept {
    static thread_local const char token = 0;
    return &token;
}

} // namespace elio::io::detail
