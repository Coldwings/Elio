#pragma once

#include <cstddef>

namespace elio::http {

/// Opt-in Transport bounds. Zero denies the corresponding resource; it never
/// means unlimited. Live connections include idle, leased, and reserved dials.
struct pool_limits {
    size_t max_idle_per_route = 6;
    size_t max_idle_total = 64;
    size_t max_live_per_route = 12;
    size_t max_live_total = 128;
    size_t max_dials_total = 16;
    size_t max_waiters_total = 256;
    size_t max_route_buckets = 128;
};

} // namespace elio::http
