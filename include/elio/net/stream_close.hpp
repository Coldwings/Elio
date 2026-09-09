#pragma once

namespace elio::net {

enum class close_scope { write_direction, whole_session };

/// Protocol closure outcome, not proof of peer receipt or lossless delivery.
struct write_finish_result {
    close_scope scope = close_scope::write_direction;
    int error = 0; ///< Positive errno, or zero on successful local completion.
    bool local_end_flushed = false;
    bool peer_end_observed = false;
};

} // namespace elio::net
