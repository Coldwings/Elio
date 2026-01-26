#pragma once

#include "promise_base.hpp"
#include <vector>
#include <string>
#include <coroutine>
#include <fmt/format.h>

namespace elio::coro {

/// Get the current virtual stack depth
inline size_t get_stack_depth() noexcept {
    size_t depth = 0;
    promise_base* frame = promise_base::current_frame();
    while (frame) {
        ++depth;
        frame = frame->parent();
    }
    return depth;
}

/// Dump the virtual stack for debugging
inline std::vector<std::string> dump_virtual_stack() {
    std::vector<std::string> frames;
    promise_base* frame = promise_base::current_frame();
    size_t index = 0;
    while (frame) {
        frames.push_back(fmt::format("Frame {}: {}", index++, static_cast<void*>(frame)));
        frame = frame->parent();
    }
    return frames;
}

/// Log the virtual stack for debugging
inline void log_virtual_stack() {
    // No-op in optimized builds
}

/// Extract promise_base pointer from a coroutine handle address
/// 
/// This function attempts to extract the promise_base from a type-erased
/// coroutine handle. It relies on the standard coroutine frame layout where
/// the promise follows the resume and destroy function pointers.
/// 
/// @param handle_addr The coroutine handle address (from handle.address())
/// @return Pointer to promise_base if valid Elio coroutine, nullptr otherwise
/// 
/// @note This uses implementation-specific knowledge of coroutine frame layout
///       but is portable across GCC and Clang. The frame layout is:
///       [resume_fn_ptr][destroy_fn_ptr][promise...]
inline promise_base* get_promise_base(void* handle_addr) noexcept {
    if (!handle_addr) return nullptr;
    
    // The coroutine frame layout has the promise after two function pointers
    // (resume and destroy). This is consistent across GCC and Clang.
    constexpr size_t promise_offset = 2 * sizeof(void*);
    
    auto* candidate = reinterpret_cast<promise_base*>(
        static_cast<char*>(handle_addr) + promise_offset);
    
    // Validate using the magic number
    if (candidate->frame_magic() == promise_base::FRAME_MAGIC) {
        return candidate;
    }
    
    return nullptr;
}

/// Check if a coroutine has affinity for a specific worker
/// 
/// @param handle_addr Coroutine handle address
/// @param worker_id Worker ID to check against
/// @return true if the coroutine has affinity for the specified worker,
///         or has no affinity (NO_AFFINITY)
inline bool check_affinity_allows(void* handle_addr, size_t worker_id) noexcept {
    auto* promise = get_promise_base(handle_addr);
    if (!promise) {
        // Not an Elio coroutine or invalid - allow execution anywhere
        return true;
    }
    
    size_t affinity = promise->affinity();
    return affinity == NO_AFFINITY || affinity == worker_id;
}

/// Get the affinity of a coroutine from its handle address
/// 
/// @param handle_addr Coroutine handle address
/// @return The worker affinity, or NO_AFFINITY if not set or invalid
inline size_t get_affinity(void* handle_addr) noexcept {
    auto* promise = get_promise_base(handle_addr);
    return promise ? promise->affinity() : NO_AFFINITY;
}

} // namespace elio::coro
