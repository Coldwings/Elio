#pragma once

#include <type_traits>

namespace elio::runtime {

// Trigger base class (for type checking)
struct trigger_base {
    using signature = void;  // placeholder
};

// on_overload: triggered when queue length exceeds threshold
// Args: (Scheduler*, size_t pending_tasks)
template<typename... Actions>
struct on_overload : trigger_base {
    static constexpr bool has_actions = (sizeof...(Actions) > 0);
};

// on_idle: triggered when queue length below threshold for sustained period
// Args: (Scheduler*, size_t pending_tasks, std::chrono::seconds idle_time)
template<typename... Actions>
struct on_idle : trigger_base {
    static constexpr bool has_actions = (sizeof...(Actions) > 0);
};

// on_block: triggered when worker has no task progress for threshold time
// Args: (Scheduler*, size_t worker_id, std::chrono::milliseconds blocked_time)
template<typename... Actions>
struct on_block : trigger_base {
    static constexpr bool has_actions = (sizeof...(Actions) > 0);
};

} // namespace elio::runtime
