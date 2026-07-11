#pragma once

/// @file websocket_frame.hpp
/// @brief WebSocket protocol framing implementation (RFC 6455)
///
/// This file provides the low-level WebSocket frame encoding/decoding,
/// including support for text, binary, ping/pong, and close frames.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <array>
#include <stdexcept>
#include <sys/random.h>

namespace elio::http::websocket {

/// WebSocket opcodes (RFC 6455 Section 5.2)
enum class opcode : uint8_t {
    continuation = 0x0,  ///< Continuation frame
    text = 0x1,          ///< Text frame (UTF-8)
    binary = 0x2,        ///< Binary frame
    // 0x3-0x7 reserved for further non-control frames
    close = 0x8,         ///< Connection close
    ping = 0x9,          ///< Ping
    pong = 0xA,          ///< Pong
    // 0xB-0xF reserved for further control frames
};

/// Check if opcode is a control frame
inline constexpr bool is_control_frame(opcode op) noexcept {
    return static_cast<uint8_t>(op) >= 0x8;
}

/// Check if opcode is valid
inline constexpr bool is_valid_opcode(uint8_t op) noexcept {
    return op <= 0x2 || (op >= 0x8 && op <= 0xA);
}

/// WebSocket close status codes (RFC 6455 Section 7.4.1)
enum class close_code : uint16_t {
    normal = 1000,           ///< Normal closure
    going_away = 1001,       ///< Endpoint going away
    protocol_error = 1002,   ///< Protocol error
    unsupported = 1003,      ///< Unsupported data type
    // 1004 reserved
    no_status = 1005,        ///< No status received (internal use)
    abnormal = 1006,         ///< Abnormal closure (internal use)
    invalid_data = 1007,     ///< Invalid frame payload data
    policy_violation = 1008, ///< Policy violation
    too_large = 1009,        ///< Message too big
    extension_required = 1010, ///< Extension required
    unexpected = 1011,       ///< Unexpected condition
    // 1015 TLS handshake failure (internal use)
};

/// Get human-readable reason for close code
inline constexpr std::string_view close_reason(close_code code) noexcept {
    switch (code) {
        case close_code::normal: return "Normal closure";
        case close_code::going_away: return "Going away";
        case close_code::protocol_error: return "Protocol error";
        case close_code::unsupported: return "Unsupported data";
        case close_code::no_status: return "No status received";
        case close_code::abnormal: return "Abnormal closure";
        case close_code::invalid_data: return "Invalid data";
        case close_code::policy_violation: return "Policy violation";
        case close_code::too_large: return "Message too big";
        case close_code::extension_required: return "Extension required";
        case close_code::unexpected: return "Unexpected condition";
        default: return "Unknown";
    }
}

/// WebSocket frame header
struct frame_header {
    bool fin = true;         ///< Final fragment flag
    bool rsv1 = false;       ///< Reserved bit 1 (for extensions)
    bool rsv2 = false;       ///< Reserved bit 2 (for extensions)
    bool rsv3 = false;       ///< Reserved bit 3 (for extensions)
    opcode op = opcode::text; ///< Opcode
    bool masked = false;     ///< Masking flag
    uint64_t payload_len = 0; ///< Payload length
    std::array<uint8_t, 4> mask_key = {}; ///< Masking key (if masked)
};

/// Frame parse result
struct frame_parse_result {
    bool complete = false;    ///< Frame is complete
    bool error = false;       ///< Parse error occurred
    size_t header_size = 0;   ///< Header size in bytes
    size_t frame_size = 0;    ///< Total frame size (header + payload)
    std::string error_msg;    ///< Error message if error is true
};

/// Parse a WebSocket frame header from raw bytes
/// @param data Input data buffer
/// @param len Length of input data
/// @param header Output header structure
/// @return Parse result
inline frame_parse_result parse_frame_header(const uint8_t* data, size_t len, frame_header& header) {
    frame_parse_result result;
    
    if (len < 2) {
        result.complete = false;
        return result;
    }
    
    // First byte: FIN, RSV1-3, opcode
    header.fin = (data[0] & 0x80) != 0;
    header.rsv1 = (data[0] & 0x40) != 0;
    header.rsv2 = (data[0] & 0x20) != 0;
    header.rsv3 = (data[0] & 0x10) != 0;
    
    uint8_t raw_opcode = data[0] & 0x0F;
    if (!is_valid_opcode(raw_opcode)) {
        result.error = true;
        result.error_msg = "Invalid opcode";
        return result;
    }
    header.op = static_cast<opcode>(raw_opcode);
    
    // Check reserved bits (must be 0 unless extensions are negotiated)
    if (header.rsv1 || header.rsv2 || header.rsv3) {
        result.error = true;
        result.error_msg = "Reserved bits must be 0";
        return result;
    }
    
    // Second byte: MASK, payload length
    header.masked = (data[1] & 0x80) != 0;
    uint8_t payload_len_7 = data[1] & 0x7F;
    
    size_t header_size = 2;
    
    if (payload_len_7 == 126) {
        // Extended 16-bit length
        if (len < 4) {
            result.complete = false;
            return result;
        }
        header.payload_len = (static_cast<uint64_t>(data[2]) << 8) |
                             static_cast<uint64_t>(data[3]);
        header_size = 4;
    } else if (payload_len_7 == 127) {
        // Extended 64-bit length
        if (len < 10) {
            result.complete = false;
            return result;
        }
        header.payload_len = (static_cast<uint64_t>(data[2]) << 56) |
                             (static_cast<uint64_t>(data[3]) << 48) |
                             (static_cast<uint64_t>(data[4]) << 40) |
                             (static_cast<uint64_t>(data[5]) << 32) |
                             (static_cast<uint64_t>(data[6]) << 24) |
                             (static_cast<uint64_t>(data[7]) << 16) |
                             (static_cast<uint64_t>(data[8]) << 8) |
                             static_cast<uint64_t>(data[9]);
        header_size = 10;
        
        // Most significant bit must be 0
        if (header.payload_len & 0x8000000000000000ULL) {
            result.error = true;
            result.error_msg = "Invalid payload length";
            return result;
        }
    } else {
        header.payload_len = payload_len_7;
    }
    
    // Control frames cannot have payload > 125 bytes
    if (is_control_frame(header.op) && header.payload_len > 125) {
        result.error = true;
        result.error_msg = "Control frame payload too large";
        return result;
    }
    
    // Control frames must not be fragmented
    if (is_control_frame(header.op) && !header.fin) {
        result.error = true;
        result.error_msg = "Control frame cannot be fragmented";
        return result;
    }
    
    // Read mask key if present
    if (header.masked) {
        if (len < header_size + 4) {
            result.complete = false;
            return result;
        }
        std::memcpy(header.mask_key.data(), data + header_size, 4);
        header_size += 4;
    }
    
    result.complete = true;
    result.header_size = header_size;
    result.frame_size = header_size + header.payload_len;
    return result;
}

/// Apply or remove XOR masking to payload data
/// @param data Data buffer to mask/unmask (in-place)
/// @param len Length of data
/// @param mask_key 4-byte masking key
/// @param offset Starting offset for mask calculation (for streaming)
inline void apply_mask(uint8_t* data, size_t len, const uint8_t* mask_key, size_t offset = 0) {
    for (size_t i = 0; i < len; ++i) {
        data[i] ^= mask_key[(offset + i) % 4];
    }
}

/// Generate a random 4-byte masking key using a CSPRNG.
/// RFC 6455 §5.3 requires masking keys to be derived from a strong source
/// of entropy and to be unpredictable.
inline std::array<uint8_t, 4> generate_mask_key() {
    std::array<uint8_t, 4> key{};
    ssize_t n = ::getrandom(key.data(), key.size(), 0);
    if (n < static_cast<ssize_t>(key.size())) {
        // Fallback: should never happen on Linux 3.17+ with valid args
        // Use /dev/urandom as last resort
        FILE* f = fopen("/dev/urandom", "rb");
        if (f) {
            auto rd = fread(key.data(), 1, key.size(), f);
            fclose(f);
            if (rd != key.size()) {
                throw std::runtime_error("websocket: failed to read masking key from /dev/urandom");
            }
        } else {
            throw std::runtime_error("websocket: no entropy source available for masking key");
        }
    }
    return key;
}

/// Encode a WebSocket frame
/// @param header Frame header
/// @param payload Payload data (will be masked if header.masked is true)
/// @return Encoded frame as bytes
inline std::vector<uint8_t> encode_frame(const frame_header& header, std::string_view payload) {
    if (is_control_frame(header.op)) {
        if (!header.fin) {
            throw std::invalid_argument("websocket: control frame cannot be fragmented");
        }
        if (payload.size() > 125) {
            throw std::invalid_argument("websocket: control frame payload too large");
        }
    }

    std::vector<uint8_t> frame;
    
    // Calculate header size
    size_t header_size = 2;
    if (payload.size() > 125) {
        if (payload.size() <= 65535) {
            header_size += 2;
        } else {
            header_size += 8;
        }
    }
    if (header.masked) {
        header_size += 4;
    }
    
    frame.reserve(header_size + payload.size());
    
    // First byte: FIN, RSV1-3, opcode
    uint8_t byte1 = static_cast<uint8_t>(header.op);
    if (header.fin) byte1 |= 0x80;
    if (header.rsv1) byte1 |= 0x40;
    if (header.rsv2) byte1 |= 0x20;
    if (header.rsv3) byte1 |= 0x10;
    frame.push_back(byte1);
    
    // Second byte: MASK, payload length
    uint8_t byte2 = header.masked ? 0x80 : 0x00;
    
    if (payload.size() <= 125) {
        byte2 |= static_cast<uint8_t>(payload.size());
        frame.push_back(byte2);
    } else if (payload.size() <= 65535) {
        byte2 |= 126;
        frame.push_back(byte2);
        frame.push_back(static_cast<uint8_t>(payload.size() >> 8));
        frame.push_back(static_cast<uint8_t>(payload.size()));
    } else {
        byte2 |= 127;
        frame.push_back(byte2);
        uint64_t len = payload.size();
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>(len >> (i * 8)));
        }
    }
    
    // Mask key (if masking)
    std::array<uint8_t, 4> mask_key = {};
    if (header.masked) {
        mask_key = header.mask_key;
        if (mask_key == std::array<uint8_t, 4>{}) {
            mask_key = generate_mask_key();
        }
        frame.insert(frame.end(), mask_key.begin(), mask_key.end());
    }
    
    // Payload (masked if needed)
    size_t payload_start = frame.size();
    frame.insert(frame.end(), payload.begin(), payload.end());
    
    if (header.masked) {
        apply_mask(frame.data() + payload_start, payload.size(), mask_key.data());
    }
    
    return frame;
}

/// Encode a text frame
inline std::vector<uint8_t> encode_text_frame(std::string_view text, bool mask = false) {
    frame_header header;
    header.op = opcode::text;
    header.fin = true;
    header.masked = mask;
    if (mask) header.mask_key = generate_mask_key();
    return encode_frame(header, text);
}

/// Encode a binary frame
inline std::vector<uint8_t> encode_binary_frame(std::string_view data, bool mask = false) {
    frame_header header;
    header.op = opcode::binary;
    header.fin = true;
    header.masked = mask;
    if (mask) header.mask_key = generate_mask_key();
    return encode_frame(header, data);
}

/// Encode a close frame.
/// RFC 6455 §7.4.1: codes 1005, 1006, and 1015 MUST NOT be sent on the wire.
/// They are silently mapped to protocol_error if provided.
inline std::vector<uint8_t> encode_close_frame(close_code code = close_code::normal,
                                                std::string_view reason = "",
                                                bool mask = false) {
    uint16_t raw = static_cast<uint16_t>(code);
    // RFC 6455 §7.4.1: 1005 (no_status), 1006 (abnormal), 1015 (TLS handshake)
    // are reserved and MUST NOT be sent in a close frame.
    if (raw == 1005 || raw == 1006 || raw == 1015) {
        code = close_code::protocol_error;
    }

    frame_header header;
    header.op = opcode::close;
    header.fin = true;
    header.masked = mask;
    if (mask) header.mask_key = generate_mask_key();

    std::string payload;
    uint16_t wire_code = static_cast<uint16_t>(code);
    payload.push_back(static_cast<char>(wire_code >> 8));
    payload.push_back(static_cast<char>(wire_code));
    if (!reason.empty()) {
        payload.append(reason);
    }

    return encode_frame(header, payload);
}

/// Encode a ping frame
inline std::vector<uint8_t> encode_ping_frame(std::string_view payload = "", bool mask = false) {
    frame_header header;
    header.op = opcode::ping;
    header.fin = true;
    header.masked = mask;
    if (mask) header.mask_key = generate_mask_key();
    return encode_frame(header, payload);
}

/// Encode a pong frame
inline std::vector<uint8_t> encode_pong_frame(std::string_view payload = "", bool mask = false) {
    frame_header header;
    header.op = opcode::pong;
    header.fin = true;
    header.masked = mask;
    if (mask) header.mask_key = generate_mask_key();
    return encode_frame(header, payload);
}

/// WebSocket message (may span multiple frames)
struct message {
    opcode type = opcode::text; ///< Message type (text or binary)
    std::string data;           ///< Message data
    bool complete = false;      ///< Message is complete
};

/// Endpoint role used for direction-dependent frame validation.
/// RFC 6455 §5.1 requires server endpoints to reject unmasked client frames
/// and client endpoints to reject masked server frames.
enum class endpoint_role {
    unspecified,  ///< No mask-direction enforcement (e.g. for unit tests on raw frames)
    server,       ///< Endpoint is acting as a server: incoming frames must be masked
    client,       ///< Endpoint is acting as a client: incoming frames must NOT be masked
};

/// Simple UTF-8 validation.  Returns true if the string is valid UTF-8.
inline bool is_valid_utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        auto c = static_cast<uint8_t>(s[i]);
        size_t len = 0;
        if (c <= 0x7F) { len = 1; }
        else if ((c & 0xE0) == 0xC0) { len = 2; }
        else if ((c & 0xF0) == 0xE0) { len = 3; }
        else if ((c & 0xF8) == 0xF0) { len = 4; }
        else { return false; }
        if (len == 0 || i + len > s.size()) return false;
        // Check continuation bytes
        for (size_t j = 1; j < len; ++j) {
            if ((static_cast<uint8_t>(s[i + j]) & 0xC0) != 0x80) return false;
        }
        // Reject overlong encodings
        if (len == 2 && c < 0xC2) return false;
        if (len == 3) {
            if (c == 0xE0 && static_cast<uint8_t>(s[i+1]) < 0xA0) return false;
            // Reject UTF-16 surrogate code points (U+D800–U+DFFF) per RFC 3629
            if (c == 0xED && static_cast<uint8_t>(s[i+1]) >= 0xA0) return false;
        }
        if (len == 4) {
            if (c == 0xF0 && static_cast<uint8_t>(s[i+1]) < 0x90) return false;
            if (c == 0xF4 && static_cast<uint8_t>(s[i+1]) > 0x8F) return false;
            if (c > 0xF4) return false;
        }
        i += len;
    }
    return true;
}

/// WebSocket frame parser with message assembly
class frame_parser {
public:
    frame_parser() = default;

    /// Set maximum message size (0 = unlimited)
    void set_max_message_size(size_t max_size) {
        max_message_size_ = max_size;
    }

    /// Set the local endpoint role so the parser can enforce RFC 6455 §5.1
    /// mask-direction rules on incoming frames.
    void set_role(endpoint_role role) { role_ = role; }

    /// Get the close code associated with the most recent error (defaults to
    /// protocol_error when has_error()).  Useful for emitting an RFC-compliant
    /// close frame before tearing down the connection.
    close_code error_close_code() const noexcept { return error_close_code_; }
    
    /// Parse incoming data
    /// @param data Input data
    /// @param len Length of input data
    /// @return Number of bytes consumed (0 if incomplete, <0 on error).
    ///         Returned as ssize_t so large consumed counts cannot overflow and
    ///         be misread as the negative error sentinel.
    ssize_t parse(const uint8_t* data, size_t len) {
        if (len == 0) return 0;

        // Append to buffer
        buffer_.insert(buffer_.end(), data, data + len);

        return process_buffer();
    }
    
    /// Check if a complete message is available
    bool has_message() const { return !messages_.empty(); }
    
    /// Get next complete message (removes from queue)
    std::optional<message> get_message() {
        if (messages_.empty()) return std::nullopt;
        auto msg = std::move(messages_.front());
        messages_.erase(messages_.begin());
        return msg;
    }
    
    /// Check if a control frame is available
    bool has_control_frame() const { return !control_frames_.empty(); }
    
    /// Get next control frame (removes from queue)
    std::optional<std::pair<opcode, std::string>> get_control_frame() {
        if (control_frames_.empty()) return std::nullopt;
        auto frame = std::move(control_frames_.front());
        control_frames_.erase(control_frames_.begin());
        return frame;
    }
    
    /// Check if error occurred
    bool has_error() const { return has_error_; }
    
    /// Get error message
    const std::string& error() const { return error_msg_; }
    
    /// Reset parser state
    void reset() {
        buffer_.clear();
        current_message_ = message{};
        messages_.clear();
        control_frames_.clear();
        has_error_ = false;
        has_initial_frame_ = false;
        error_msg_.clear();
        error_close_code_ = close_code::protocol_error;
    }
    
private:
    ssize_t process_buffer() {
        size_t total_consumed = 0;
        
        while (buffer_.size() >= 2) {
            frame_header header;
            auto result = parse_frame_header(buffer_.data(), buffer_.size(), header);
            
            if (result.error) {
                has_error_ = true;
                error_msg_ = result.error_msg;
                error_close_code_ = close_code::protocol_error;
                return -1;
            }

            if (!result.complete) {
                // Need more data
                break;
            }

            // Validate fragmentation state as soon as the complete header is
            // known — before the full payload arrives — so that protocol errors
            // are caught early regardless of whether a size limit is configured.
            if (!is_control_frame(header.op)) {
                if (header.op == opcode::continuation) {
                    // Continuation frame: RFC 6455 §5.4 requires a preceding
                    // initial text/binary frame.  If none exists, this is a
                    // protocol error even before its payload arrives.
                    if (!has_initial_frame_) {
                        has_error_ = true;
                        error_msg_ = "Continuation frame without initial frame";
                        error_close_code_ = close_code::protocol_error;
                        return -1;
                    }
                } else if (has_initial_frame_) {
                    has_error_ = true;
                    error_msg_ = "New message started before previous completed";
                    error_close_code_ = close_code::protocol_error;
                    return -1;
                }

                // Reject impossible data-message growth as soon as the complete
                // header reveals the payload length. This keeps a peer from
                // forcing buffer_ to grow toward an already-invalid frame size.
                if (max_message_size_ > 0) {
                    size_t current_size = current_message_.data.size();
                    if (current_size > max_message_size_ ||
                        header.payload_len >
                            static_cast<uint64_t>(max_message_size_ - current_size)) {
                        has_error_ = true;
                        error_msg_ = "Message exceeds maximum size";
                        error_close_code_ = close_code::too_large;
                        return -1;
                    }
                }
            }

            if (buffer_.size() < result.frame_size) {
                // Need more payload data
                break;
            }

            // RFC 6455 §5.1: enforce mask direction once we know our role.
            // Servers MUST tear down a connection that delivers an unmasked
            // client frame; clients MUST tear down on a masked server frame.
            if (role_ == endpoint_role::server && !header.masked) {
                has_error_ = true;
                error_msg_ = "Unmasked frame received from client";
                error_close_code_ = close_code::protocol_error;
                return -1;
            }
            if (role_ == endpoint_role::client && header.masked) {
                has_error_ = true;
                error_msg_ = "Masked frame received from server";
                error_close_code_ = close_code::protocol_error;
                return -1;
            }
            
            // Helper to extract (and unmask) the frame payload from the buffer.
            // Deferred so that we can reject oversized data frames *before*
            // allocating/copying the payload.
            auto extract_payload = [&]() {
                std::string payload(
                    reinterpret_cast<const char*>(buffer_.data() + result.header_size),
                    header.payload_len
                );
                if (header.masked) {
                    apply_mask(reinterpret_cast<uint8_t*>(payload.data()),
                              payload.size(), header.mask_key.data());
                }
                return payload;
            };

            // Process frame
            if (is_control_frame(header.op)) {
                // Control frames can be interleaved.  Their payload is capped at
                // 125 bytes by parse_frame_header(), so no size check is needed.
                control_frames_.emplace_back(header.op, extract_payload());
            } else {
                // Data frame — fragmentation-state and size checks were already
                // applied in the early-validation block above, so start
                // accumulating the payload directly.
                if (header.op != opcode::continuation) {
                    // Start of new message
                    current_message_ = message{};
                    current_message_.type = header.op;
                    has_initial_frame_ = true;
                }

                // Extract (and unmask) the payload only after passing the size check.
                current_message_.data.append(extract_payload());

                if (header.fin) {
                    // RFC 6455 §8.1: text messages MUST be valid UTF-8.
                    // Validate before delivering to application.
                    if (current_message_.type == opcode::text) {
                        if (!is_valid_utf8(current_message_.data)) {
                            has_error_ = true;
                            error_msg_ = "Text message contains invalid UTF-8";
                            error_close_code_ = close_code::invalid_data;
                            return -1;
                        }
                    }
                    current_message_.complete = true;
                    messages_.push_back(std::move(current_message_));
                    current_message_ = message{};
                    has_initial_frame_ = false;
                }
            }
            
            // Remove processed frame from buffer
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(result.frame_size));
            total_consumed += result.frame_size;
        }
        
        return static_cast<ssize_t>(total_consumed);
    }
    
    std::vector<uint8_t> buffer_;
    message current_message_;
    std::vector<message> messages_;
    std::vector<std::pair<opcode, std::string>> control_frames_;
    size_t max_message_size_ = 0;
    bool has_error_ = false;
    bool has_initial_frame_ = false;  ///< Tracks whether a non-continuation frame started the current message
    std::string error_msg_;
    close_code error_close_code_ = close_code::protocol_error;
    endpoint_role role_ = endpoint_role::unspecified;
};

/// Validate a close code per RFC 6455 §7.4.1.
/// Returns true if the code is valid for use in a close frame.
inline bool is_valid_close_code(uint16_t code) {
    // Defined codes: 1000-1003, 1007-1014
    // (1012=Service Restart, 1013=Try Again Later, 1014=Bad Gateway — RFC 6455 §7.4.1)
    if (code >= 1000 && code <= 1003) return true;
    if (code >= 1007 && code <= 1014) return true;
    // Reserved for libraries/frameworks/applications: 3000-4999
    if (code >= 3000 && code <= 4999) return true;
    // Everything else (0-999, 1004, 1005-1006, 1015, 1016-2999, 5000+) is invalid
    return false;
}

/// Parse close frame payload to extract code and reason.
/// Validates the close code per RFC 6455 §7.4.1 and the reason per §7.1.6.
/// Returns protocol_error for invalid codes or non-UTF-8 reasons.
inline std::pair<close_code, std::string> parse_close_payload(std::string_view payload) {
    if (payload.empty()) {
        return {close_code::no_status, ""};
    }
    if (payload.size() == 1) {
        return {close_code::protocol_error, ""};
    }

    uint16_t code = (static_cast<uint8_t>(payload[0]) << 8) |
                    static_cast<uint8_t>(payload[1]);

    // Validate close code per RFC 6455 §7.4.1
    if (!is_valid_close_code(code)) {
        return {close_code::protocol_error, ""};
    }

    std::string reason;
    if (payload.size() > 2) {
        reason = std::string(payload.substr(2));
        // Validate UTF-8 per RFC 6455 §7.1.6
        if (!is_valid_utf8(reason)) {
            return {close_code::protocol_error, ""};
        }
    }
    
    return {static_cast<close_code>(code), reason};
}

} // namespace elio::http::websocket
