#pragma once

namespace elio::http::websocket {

/// Shared client/server WebSocket connection state.
enum class connection_state {
    connecting,  ///< Handshake in progress
    open,        ///< Connection open
    closing,     ///< Close handshake in progress
    closed       ///< Connection closed
};

} // namespace elio::http::websocket
