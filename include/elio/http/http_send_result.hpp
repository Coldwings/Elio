#pragma once

#include <cstdint>

namespace elio::http {

enum class send_errc {
    none,
    invalid_response,
    length_mismatch,
    invalid_state,
    cancelled,
    timed_out,
    transport_error,
    producer_error,
};

/// A terminal send failure may follow visible wire side effects. Byte counts
/// report observed body progress, not a remainder suitable for response replay.
struct send_result {
    send_errc error = send_errc::none;
    int transport_error = 0;
    uint64_t confirmed_body_bytes = 0;

    bool success() const noexcept { return error == send_errc::none; }
};

} // namespace elio::http
