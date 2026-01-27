#pragma once

/// @file rpc_protocol.hpp
/// @brief Wire protocol for RPC communication
///
/// This module defines the binary wire format for RPC messages:
/// - Frame header with request ID, method ID, flags, and payload length
/// - Message types (request, response, error)
/// - Optional CRC32 checksum for message integrity
/// - Stream-based framing for TCP/UDS sockets
///
/// Wire Format (all little-endian):
/// +----------+----------+-------+--------+------------+---------+
/// | magic(4) | req_id(4)| type(1)| flags(1)| method(4) | len(4)  |
/// +----------+----------+-------+--------+------------+---------+
/// | payload (len bytes)                                         |
/// +-------------------------------------------------------------+
/// | checksum(4) - optional, present if has_checksum flag set    |
/// +-------------------------------------------------------------+
///
/// Total header size: 18 bytes
/// Checksum trailer: 4 bytes (optional)

#include "rpc_buffer.hpp"
#include "rpc_types.hpp"

#include <elio/coro/task.hpp>
#include <elio/hash/crc32.hpp>
#include <elio/net/tcp.hpp>
#include <elio/net/uds.hpp>
#include <elio/log/macros.hpp>

#include <atomic>
#include <cstdint>

namespace elio::rpc {

// ============================================================================
// Protocol constants
// ============================================================================

/// Protocol magic number for validation
constexpr uint32_t protocol_magic = 0x454C494F; // "ELIO" in ASCII

/// Protocol version
constexpr uint8_t protocol_version = 1;

/// Frame header size
constexpr size_t frame_header_size = 18;

/// Checksum trailer size
constexpr size_t checksum_size = 4;

/// Default timeout for RPC calls (milliseconds)
constexpr uint32_t default_timeout_ms = 30000;

// ============================================================================
// Message types
// ============================================================================

/// Message type enumeration
enum class message_type : uint8_t {
    request = 0,       ///< RPC request
    response = 1,      ///< Successful response
    error = 2,         ///< Error response
    ping = 3,          ///< Keepalive ping
    pong = 4,          ///< Keepalive pong
    cancel = 5,        ///< Cancel pending request
};

/// Message flags
enum class message_flags : uint8_t {
    none = 0,
    has_timeout = 1 << 0,    ///< Request includes timeout value
    has_checksum = 1 << 1,   ///< Message has CRC32 checksum trailer
    compressed = 1 << 2,     ///< Payload is compressed (reserved)
    streaming = 1 << 3,      ///< Part of a streaming call (reserved)
};

inline message_flags operator|(message_flags a, message_flags b) {
    return static_cast<message_flags>(
        static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline message_flags operator&(message_flags a, message_flags b) {
    return static_cast<message_flags>(
        static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline bool has_flag(message_flags flags, message_flags flag) {
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(flag)) != 0;
}

// ============================================================================
// Frame header
// ============================================================================

/// Frame header structure
struct frame_header {
    uint32_t magic = protocol_magic;  ///< Magic number for validation
    uint32_t request_id = 0;          ///< Request ID for correlation
    message_type type = message_type::request;
    message_flags flags = message_flags::none;
    method_id_t method_id = 0;        ///< Method being called (for requests)
    uint32_t payload_length = 0;      ///< Length of payload in bytes
    
    /// Validate header
    bool is_valid() const noexcept {
        return magic == protocol_magic && 
               payload_length <= max_message_size;
    }
    
    /// Serialize header to buffer
    void serialize(buffer_writer& writer) const {
        writer.write(magic);
        writer.write(request_id);
        writer.write(static_cast<uint8_t>(type));
        writer.write(static_cast<uint8_t>(flags));
        writer.write(method_id);
        writer.write(payload_length);
    }
    
    /// Serialize header to byte array
    std::array<uint8_t, frame_header_size> to_bytes() const {
        std::array<uint8_t, frame_header_size> bytes;
        uint8_t* p = bytes.data();
        std::memcpy(p, &magic, 4); p += 4;
        std::memcpy(p, &request_id, 4); p += 4;
        *p++ = static_cast<uint8_t>(type);
        *p++ = static_cast<uint8_t>(flags);
        std::memcpy(p, &method_id, 4); p += 4;
        std::memcpy(p, &payload_length, 4);
        return bytes;
    }
    
    /// Deserialize header from buffer view
    static frame_header deserialize(buffer_view& reader) {
        frame_header h;
        h.magic = reader.read<uint32_t>();
        h.request_id = reader.read<uint32_t>();
        h.type = static_cast<message_type>(reader.read<uint8_t>());
        h.flags = static_cast<message_flags>(reader.read<uint8_t>());
        h.method_id = reader.read<method_id_t>();
        h.payload_length = reader.read<uint32_t>();
        return h;
    }
    
    /// Deserialize from byte array
    static frame_header from_bytes(const uint8_t* data) {
        frame_header h;
        const uint8_t* p = data;
        std::memcpy(&h.magic, p, 4); p += 4;
        std::memcpy(&h.request_id, p, 4); p += 4;
        h.type = static_cast<message_type>(*p++);
        h.flags = static_cast<message_flags>(*p++);
        std::memcpy(&h.method_id, p, 4); p += 4;
        std::memcpy(&h.payload_length, p, 4);
        return h;
    }
};

static_assert(frame_header_size == 18, "Header size mismatch");

// ============================================================================
// Error response payload
// ============================================================================

/// Error response structure
struct error_payload {
    rpc_error code = rpc_error::internal_error;
    std::string message;
    
    ELIO_RPC_FIELDS(error_payload, code, message)
};

// ============================================================================
// Request ID generator
// ============================================================================

/// Thread-safe request ID generator
class request_id_generator {
public:
    uint32_t next() noexcept {
        return counter_.fetch_add(1, std::memory_order_relaxed);
    }
    
private:
    std::atomic<uint32_t> counter_{1};
};

// ============================================================================
// Stream protocol helpers
// ============================================================================

/// Stream concept for TCP or UDS streams
template<typename T>
concept rpc_stream = requires(T& stream, void* buf, const void* cbuf, size_t len, 
                              struct iovec* iovecs, size_t iov_count) {
    { stream.read(buf, len) };
    { stream.write(cbuf, len) };
    { stream.writev(iovecs, iov_count) };
    { stream.is_valid() } -> std::same_as<bool>;
};

/// Read exactly n bytes from stream
template<rpc_stream Stream>
coro::task<io::io_result> read_exact(Stream& stream, void* buffer, size_t length) {
    uint8_t* ptr = static_cast<uint8_t*>(buffer);
    size_t remaining = length;
    
    while (remaining > 0) {
        auto result = co_await stream.read(ptr, remaining);
        if (result.result <= 0) {
            co_return result;
        }
        ptr += result.result;
        remaining -= result.result;
    }
    
    co_return io::io_result{static_cast<int32_t>(length), 0};
}

/// Write exactly n bytes to stream
template<rpc_stream Stream>
coro::task<io::io_result> write_exact(Stream& stream, const void* buffer, size_t length) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buffer);
    size_t remaining = length;
    
    while (remaining > 0) {
        auto result = co_await stream.write(ptr, remaining);
        if (result.result <= 0) {
            co_return result;
        }
        ptr += result.result;
        remaining -= result.result;
    }
    
    co_return io::io_result{static_cast<int32_t>(length), 0};
}

/// Write all data from iovec array to stream (scatter-gather write)
/// Handles partial writes by adjusting iovec entries
template<rpc_stream Stream>
coro::task<io::io_result> writev_exact(Stream& stream, struct iovec* iovecs, size_t iov_count) {
    size_t total_length = 0;
    for (size_t i = 0; i < iov_count; ++i) {
        total_length += iovecs[i].iov_len;
    }
    
    size_t current_iov = 0;
    size_t bytes_written = 0;
    
    while (current_iov < iov_count) {
        auto result = co_await stream.writev(&iovecs[current_iov], iov_count - current_iov);
        if (result.result <= 0) {
            co_return result;
        }
        
        bytes_written += result.result;
        size_t written = static_cast<size_t>(result.result);
        
        // Advance through iovecs based on how much was written
        while (written > 0 && current_iov < iov_count) {
            if (written >= iovecs[current_iov].iov_len) {
                written -= iovecs[current_iov].iov_len;
                ++current_iov;
            } else {
                // Partial write within this iovec entry
                iovecs[current_iov].iov_base = 
                    static_cast<uint8_t*>(iovecs[current_iov].iov_base) + written;
                iovecs[current_iov].iov_len -= written;
                written = 0;
            }
        }
    }
    
    co_return io::io_result{static_cast<int32_t>(total_length), 0};
}

/// Read a complete frame from stream
/// If the frame has the has_checksum flag set, verifies the CRC32 checksum
template<rpc_stream Stream>
coro::task<std::optional<std::pair<frame_header, message_buffer>>> 
read_frame(Stream& stream) {
    // Read header
    std::array<uint8_t, frame_header_size> header_buf;
    auto result = co_await read_exact(stream, header_buf.data(), frame_header_size);
    if (result.result <= 0) {
        co_return std::nullopt;
    }
    
    // Parse header
    frame_header header = frame_header::from_bytes(header_buf.data());
    if (!header.is_valid()) {
        ELIO_LOG_ERROR("Invalid frame header: magic={:08x}, len={}", 
                      header.magic, header.payload_length);
        co_return std::nullopt;
    }
    
    // Read payload
    message_buffer payload(header.payload_length);
    if (header.payload_length > 0) {
        result = co_await read_exact(stream, payload.data(), header.payload_length);
        if (result.result <= 0) {
            co_return std::nullopt;
        }
    }
    
    // Verify checksum if present
    if (has_flag(header.flags, message_flags::has_checksum)) {
        // Read checksum trailer
        uint32_t received_checksum = 0;
        result = co_await read_exact(stream, &received_checksum, checksum_size);
        if (result.result <= 0) {
            co_return std::nullopt;
        }
        
        // Compute checksum over header + payload
        uint32_t crc = hash::crc32_update(header_buf.data(), frame_header_size, 0xFFFFFFFF);
        if (header.payload_length > 0) {
            crc = hash::crc32_update(payload.data(), header.payload_length, crc);
        }
        uint32_t computed = hash::crc32_finalize(crc);
        
        if (computed != received_checksum) {
            ELIO_LOG_ERROR("Frame checksum mismatch: expected={:08x}, received={:08x}",
                          computed, received_checksum);
            co_return std::nullopt;
        }
    }
    
    co_return std::make_pair(header, std::move(payload));
}

/// Write a frame to stream using scatter-gather I/O for atomicity
/// If the header has the has_checksum flag set, appends CRC32 checksum
template<rpc_stream Stream>
coro::task<bool> write_frame(Stream& stream, const frame_header& header, 
                              const buffer_writer& payload) {
    // Prepare header bytes
    auto header_bytes = header.to_bytes();
    
    // Compute checksum if needed (must be done before building iovecs)
    uint32_t checksum = 0;
    if (has_flag(header.flags, message_flags::has_checksum)) {
        uint32_t crc = hash::crc32_update(header_bytes.data(), frame_header_size, 0xFFFFFFFF);
        if (payload.size() > 0) {
            crc = hash::crc32_update(payload.data(), payload.size(), crc);
        }
        checksum = hash::crc32_finalize(crc);
    }
    
    // Build iovec array for atomic write
    struct iovec iovecs[3];
    size_t iov_count = 0;
    
    // Header
    iovecs[iov_count].iov_base = header_bytes.data();
    iovecs[iov_count].iov_len = frame_header_size;
    ++iov_count;
    
    // Payload
    if (payload.size() > 0) {
        iovecs[iov_count].iov_base = const_cast<uint8_t*>(payload.data());
        iovecs[iov_count].iov_len = payload.size();
        ++iov_count;
    }
    
    // Checksum
    if (has_flag(header.flags, message_flags::has_checksum)) {
        iovecs[iov_count].iov_base = &checksum;
        iovecs[iov_count].iov_len = checksum_size;
        ++iov_count;
    }
    
    // Write all data atomically using scatter-gather I/O
    auto result = co_await writev_exact(stream, iovecs, iov_count);
    co_return result.result > 0;
}

/// Write a frame with raw payload using scatter-gather I/O for atomicity
/// If the header has the has_checksum flag set, appends CRC32 checksum
template<rpc_stream Stream>
coro::task<bool> write_frame(Stream& stream, const frame_header& header,
                              const void* payload_data, size_t payload_size) {
    // Prepare header bytes
    auto header_bytes = header.to_bytes();
    
    // Compute checksum if needed
    uint32_t checksum = 0;
    if (has_flag(header.flags, message_flags::has_checksum)) {
        uint32_t crc = hash::crc32_update(header_bytes.data(), frame_header_size, 0xFFFFFFFF);
        if (payload_size > 0) {
            crc = hash::crc32_update(payload_data, payload_size, crc);
        }
        checksum = hash::crc32_finalize(crc);
    }
    
    // Build iovec array for atomic write
    struct iovec iovecs[3];
    size_t iov_count = 0;
    
    // Header
    iovecs[iov_count].iov_base = header_bytes.data();
    iovecs[iov_count].iov_len = frame_header_size;
    ++iov_count;
    
    // Payload
    if (payload_size > 0) {
        iovecs[iov_count].iov_base = const_cast<void*>(payload_data);
        iovecs[iov_count].iov_len = payload_size;
        ++iov_count;
    }
    
    // Checksum
    if (has_flag(header.flags, message_flags::has_checksum)) {
        iovecs[iov_count].iov_base = &checksum;
        iovecs[iov_count].iov_len = checksum_size;
        ++iov_count;
    }
    
    // Write all data atomically using scatter-gather I/O
    auto result = co_await writev_exact(stream, iovecs, iov_count);
    co_return result.result > 0;
}

// ============================================================================
// Message builders
// ============================================================================

/// Build a request frame
template<typename Request>
std::pair<frame_header, buffer_writer> build_request(
    uint32_t request_id,
    method_id_t method_id,
    const Request& request,
    std::optional<uint32_t> timeout_ms = std::nullopt,
    bool enable_checksum = false)
{
    buffer_writer payload;
    
    // Write timeout if specified
    message_flags flags = message_flags::none;
    if (timeout_ms) {
        flags = flags | message_flags::has_timeout;
        payload.write(*timeout_ms);
    }
    if (enable_checksum) {
        flags = flags | message_flags::has_checksum;
    }
    
    // Serialize request
    serialize(payload, request);
    
    frame_header header;
    header.request_id = request_id;
    header.type = message_type::request;
    header.flags = flags;
    header.method_id = method_id;
    header.payload_length = static_cast<uint32_t>(payload.size());
    
    return {header, std::move(payload)};
}

/// Build a response frame
template<typename Response>
std::pair<frame_header, buffer_writer> build_response(
    uint32_t request_id,
    const Response& response,
    bool enable_checksum = false)
{
    buffer_writer payload;
    serialize(payload, response);
    
    frame_header header;
    header.request_id = request_id;
    header.type = message_type::response;
    header.flags = enable_checksum ? message_flags::has_checksum : message_flags::none;
    header.method_id = 0;
    header.payload_length = static_cast<uint32_t>(payload.size());
    
    return {header, std::move(payload)};
}

/// Build an error response frame
inline std::pair<frame_header, buffer_writer> build_error_response(
    uint32_t request_id,
    rpc_error error_code,
    std::string_view error_message = "",
    bool enable_checksum = false)
{
    buffer_writer payload;
    error_payload err{error_code, std::string(error_message)};
    serialize(payload, err);
    
    frame_header header;
    header.request_id = request_id;
    header.type = message_type::error;
    header.flags = enable_checksum ? message_flags::has_checksum : message_flags::none;
    header.method_id = 0;
    header.payload_length = static_cast<uint32_t>(payload.size());
    
    return {header, std::move(payload)};
}

/// Build a ping frame
inline frame_header build_ping(uint32_t ping_id) {
    frame_header header;
    header.request_id = ping_id;
    header.type = message_type::ping;
    header.flags = message_flags::none;
    header.method_id = 0;
    header.payload_length = 0;
    return header;
}

/// Build a pong frame
inline frame_header build_pong(uint32_t ping_id) {
    frame_header header;
    header.request_id = ping_id;
    header.type = message_type::pong;
    header.flags = message_flags::none;
    header.method_id = 0;
    header.payload_length = 0;
    return header;
}

/// Build a cancel frame
inline frame_header build_cancel(uint32_t request_id) {
    frame_header header;
    header.request_id = request_id;
    header.type = message_type::cancel;
    header.flags = message_flags::none;
    header.method_id = 0;
    header.payload_length = 0;
    return header;
}

// ============================================================================
// Response parsing helpers
// ============================================================================

/// Parse request payload
template<typename Request>
std::pair<std::optional<uint32_t>, Request> parse_request(
    buffer_view& payload, message_flags flags)
{
    std::optional<uint32_t> timeout_ms;
    if (has_flag(flags, message_flags::has_timeout)) {
        timeout_ms = payload.read<uint32_t>();
    }
    
    Request request;
    deserialize(payload, request);
    
    return {timeout_ms, std::move(request)};
}

/// Parse response payload
template<typename Response>
Response parse_response(buffer_view& payload) {
    Response response;
    deserialize(payload, response);
    return response;
}

/// Parse error payload
inline error_payload parse_error(buffer_view& payload) {
    error_payload err;
    deserialize(payload, err);
    return err;
}

} // namespace elio::rpc
