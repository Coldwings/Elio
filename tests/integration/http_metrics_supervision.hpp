#pragma once

#include <atomic>
#include <cstdint>

namespace elio::test::http_metrics {

// Sampling a deadline is not sufficient: the worker may finish that phase
// before the supervisor checks its clock. Claim only the still-current phase.
inline bool try_claim_expired_phase(std::atomic<int64_t>& deadline,
                                    int64_t sampled_deadline,
                                    int64_t now) noexcept {
    if (sampled_deadline == 0 || now < sampled_deadline) {
        return false;
    }
    return deadline.compare_exchange_strong(sampled_deadline, 0);
}

} // namespace elio::test::http_metrics
