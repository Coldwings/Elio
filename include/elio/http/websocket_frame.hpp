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
#include <random>
#include <array>

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

/// Generate a random 4-byte masking key
inline std::array<uint8_t, 4> generate_mask_key() {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint32_t> dist;
    uint32_t key = dist(rng);
    return {
        static_cast<uint8_t>(key >> 24),
        static_cast<uint8_t>(key >> 16),
        static_cast<uint8_t>(key >> 8),
        static_cast<uint8_t>(key)
    };
}

/// Encode a WebSocket frame
/// @param header Frame header
/// @param payload Payload data (will be masked if header.masked is true)
/// @return Encoded frame as bytes
inline std::vector<uint8_t> encode_frame(const frame_header& header, std::string_view payload) {
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

/// Encode a close frame
inline std::vector<uint8_t> encode_close_frame(close_code code = close_code::normal, 
                                                std::string_view reason = "", 
                                                bool mask = false) {
    frame_header header;
    header.op = opcode::close;
    header.fin = true;
    header.masked = mask;
    if (mask) header.mask_key = generate_mask_key();
    
    std::string payload;
    payload.push_back(static_cast<char>(static_cast<uint16_t>(code) >> 8));
    payload.push_back(static_cast<char>(static_cast<uint16_t>(code)));
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

/// WebSocket frame parser with message assembly
class frame_parser {
public:
    frame_parser() = default;
    
    /// Set maximum message size (0 = unlimited)
    void set_max_message_size(size_t max_size) {
        max_message_size_ = max_size;
    }
    
    /// Parse incoming data
    /// @param data Input data
    /// @param len Length of input data
    /// @return Number of bytes consumed (0 if incomplete, <0 on error)
    int parse(const uint8_t* data, size_t len) {
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
        error_msg_.clear();
    }
    
private:
    int process_buffer() {
        size_t total_consumed = 0;
        
        while (buffer_.size() >= 2) {
            frame_header header;
            auto result = parse_frame_header(buffer_.data(), buffer_.size(), header);
            
            if (result.error) {
                has_error_ = true;
                error_msg_ = result.error_msg;
                return -1;
            }
            
            if (!result.complete) {
                // Need more data
                break;
            }
            
            if (buffer_.size() < result.frame_size) {
                // Need more payload data
                break;
            }
            
            // Extract payload
            std::string payload(
                reinterpret_cast<const char*>(buffer_.data() + result.header_size),
                header.payload_len
            );
            
            // Unmask if needed
            if (header.masked) {
                apply_mask(reinterpret_cast<uint8_t*>(payload.data()), 
                          payload.size(), header.mask_key.data());
            }
            
            // Process frame
            if (is_control_frame(header.op)) {
                // Control frames can be interleaved
                control_frames_.emplace_back(header.op, std::move(payload));
            } else {
                // Data frame
                if (header.op != opcode::continuation) {
                    // Start of new message
                    if (!current_message_.data.empty() && !current_message_.complete) {
                        has_error_ = true;
                        error_msg_ = "New message started before previous completed";
                        return -1;
                    }
                    current_message_ = message{};
                    current_message_.type = header.op;
                }
                
                // Check message size limit
                if (max_message_size_ > 0 && 
                    current_message_.data.size() + payload.size() > max_message_size_) {
                    has_error_ = true;
                    error_msg_ = "Message exceeds maximum size";
                    return -1;
                }
                
                current_message_.data.append(payload);
                
                if (header.fin) {
                    current_message_.complete = true;
                    messages_.push_back(std::move(current_message_));
                    current_message_ = message{};
                }
            }
            
            // Remove processed frame from buffer
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(result.frame_size));
            total_consumed += result.frame_size;
        }
        
        return static_cast<int>(total_consumed);
    }
    
    std::vector<uint8_t> buffer_;
    message current_message_;
    std::vector<message> messages_;
    std::vector<std::pair<opcode, std::string>> control_frames_;
    size_t max_message_size_ = 0;
    bool has_error_ = false;
    std::string error_msg_;
};

/// Parse close frame payload to extract code and reason
inline std::pair<close_code, std::string> parse_close_payload(std::string_view payload) {
    if (payload.size() < 2) {
        return {close_code::no_status, ""};
    }
    
    uint16_t code = (static_cast<uint8_t>(payload[0]) << 8) | 
                    static_cast<uint8_t>(payload[1]);
    
    std::string reason;
    if (payload.size() > 2) {
        reason = std::string(payload.substr(2));
    }
    
    return {static_cast<close_code>(code), reason};
}

} // namespace elio::http::websocket
