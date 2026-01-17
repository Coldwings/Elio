#pragma once

#include "promise_base.hpp"
#include <vector>
#include <string>
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

} // namespace elio::coro
