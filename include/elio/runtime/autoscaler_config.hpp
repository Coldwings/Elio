#pragma once

#include <chrono>
#include <cstddef>

namespace elio::runtime {

struct autoscaler_config {
    std::chrono::milliseconds tick_interval{500};           // Detection interval
    size_t overload_threshold{10};                         // Scale up threshold: queue length > 10
    size_t idle_threshold{2};                             // Scale down threshold: queue length < 2
    std::chrono::seconds idle_delay{30};                  // Low load duration before scale down
    size_t min_workers{1};                                // Minimum worker count
    size_t max_workers{std::thread::hardware_concurrency()};
    std::chrono::milliseconds block_threshold{5000};      // Block detection threshold: 5 seconds
};

} // namespace elio::runtime
