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
/// Wire Format (every multi-byte field is little-endian on the wire,
/// regardless of host byte order — see rpc_endian.hpp):
/// +----------+----------+-------+--------+------------+---------+------+
/// | magic(4) | req_id(4)| type(1)| flags(1)| method(4) | len(4)  | ver  |
/// +----------+----------+-------+--------+------------+---------+------+
/// | payload (len bytes)                                         |
/// +-------------------------------------------------------------+
/// | checksum(4) - optional, present if has_checksum flag set    |
/// +-------------------------------------------------------------+
///
/// Total header size: 19 bytes
/// Checksum trailer: 4 bytes (optional)

#include "rpc_buffer.hpp"
#include "rpc_endian.hpp"
#include "rpc_types.hpp"

#include <elio/coro/task.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/hash/crc32.hpp>

#include <algorithm>
#include <climits>
#include <sys/uio.h>  // For struct iovec and IOV_MAX

// IOV_MAX fallback for systems that don't define it
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif
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

/// Frame header size (includes version byte)
constexpr size_t frame_header_size = 19;

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
    no_response = 1 << 4,    ///< Request does not expect response/error
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

inline bool is_valid_message_type(message_type type) noexcept {
    switch (type) {
        case message_type::request:
        case message_type::response:
        case message_type::error:
        case message_type::ping:
        case message_type::pong:
        case message_type::cancel:
            return true;
    }
    return false;
}

inline bool has_only_supported_message_flags(message_flags flags) noexcept {
    constexpr uint8_t supported_flags =
        static_cast<uint8_t>(message_flags::has_timeout) |
        static_cast<uint8_t>(message_flags::has_checksum) |
        static_cast<uint8_t>(message_flags::no_response);
    const auto raw_flags = static_cast<uint8_t>(flags);
    return (raw_flags & supported_flags) == raw_flags;
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
    uint8_t version = protocol_version; ///< Protocol version

    /// Validate header
    bool is_valid() const noexcept {
        return magic == protocol_magic &&
               version == protocol_version &&
               is_valid_message_type(type) &&
               has_only_supported_message_flags(flags) &&
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
        writer.write(version);
    }

    /// Serialize header to byte array (little-endian on every host).
    std::array<uint8_t, frame_header_size> to_bytes() const {
        std::array<uint8_t, frame_header_size> bytes;
        uint8_t* p = bytes.data();
        endian::write_le<uint32_t>(p, magic); p += 4;
        endian::write_le<uint32_t>(p, request_id); p += 4;
        *p++ = static_cast<uint8_t>(type);
        *p++ = static_cast<uint8_t>(flags);
        endian::write_le<uint32_t>(p, method_id); p += 4;
        endian::write_le<uint32_t>(p, payload_length); p += 4;
        *p++ = version;
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
        h.version = reader.read<uint8_t>();
        return h;
    }

    /// Deserialize from byte array (assumes little-endian on the wire).
    static frame_header from_bytes(const uint8_t* data) {
        frame_header h;
        const uint8_t* p = data;
        h.magic = endian::read_le<uint32_t>(p); p += 4;
        h.request_id = endian::read_le<uint32_t>(p); p += 4;
        h.type = static_cast<message_type>(*p++);
        h.flags = static_cast<message_flags>(*p++);
        h.method_id = endian::read_le<uint32_t>(p); p += 4;
        h.payload_length = endian::read_le<uint32_t>(p); p += 4;
        h.version = *p++;
        return h;
    }
};

static_assert(frame_header_size == 19, "Header size mismatch");

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

/// Thread-safe request ID generator.
/// Uses a 64-bit counter internally to skip the reserved sentinel (0).
/// The wire format is uint32_t, so IDs still wrap with a period of
/// UINT32_MAX (~4.29 billion). At ~1M req/s this is ~72 minutes.
/// The 64-bit counter ensures 0 is never emitted (reserved sentinel),
/// but does NOT prevent wraparound collisions — that requires the
/// wire format to use uint64_t or a pending-ID skip mechanism.
class request_id_generator {
public:
    uint32_t next() noexcept {
        // 64-bit counter ensures we skip 0 (reserved sentinel).
        // Map to [1, UINT32_MAX] by modding against UINT32_MAX and adding 1.
        // Note: wire format is still uint32_t, so wraparound collisions
        // are possible after UINT32_MAX requests (~72 min at 1M req/s).
        uint64_t val = counter_.fetch_add(1, std::memory_order_relaxed);
        return static_cast<uint32_t>((val % UINT32_MAX) + 1);
    }

private:
    std::atomic<uint64_t> counter_{0};
};

// ============================================================================
// Stream protocol helpers
// ============================================================================

/// Stream concept for TCP or UDS streams.
///
/// RPC framing transfers whole messages, so it relies on the stream's
/// exact-length helpers (``read_exactly`` / ``write_exactly``) rather than
/// hand-rolling partial-read/write loops: a partial transfer of a frame means
/// the message is corrupt regardless. ``writev`` is still required for the
/// scatter-gather ``write_frame`` fast path.
template<typename T>
concept rpc_stream = requires(T& stream, void* buf, const void* cbuf, size_t len,
                              struct iovec* iovecs, size_t iov_count) {
    { stream.read_exactly(buf, len) };
    { stream.write_exactly(cbuf, len) };
    { stream.writev(iovecs, iov_count) };
    { stream.poll_write() };
    { stream.is_valid() } -> std::same_as<bool>;
};

template<typename T>
concept cancellable_rpc_stream = rpc_stream<T> &&
    requires(T& stream, const void* cbuf, size_t len, coro::cancel_token token) {
        { stream.write_exactly(cbuf, len, token) };
    };

/// Write all data from iovec array to stream (scatter-gather write)
/// Handles partial writes by adjusting iovec entries
template<rpc_stream Stream>
coro::task<io::io_result> writev_exact(Stream& stream, struct iovec* iovecs, size_t iov_count) {
    size_t total_length = 0;
    for (size_t i = 0; i < iov_count; ++i) {
        total_length += iovecs[i].iov_len;
    }
    
    size_t current_iov = 0;
    while (current_iov < iov_count) {
        // Batch writes to respect IOV_MAX limit (typically 1024 on Linux)
        size_t batch_size = std::min(
            static_cast<size_t>(iov_count - current_iov),
            static_cast<size_t>(IOV_MAX)
        );

        auto result = co_await stream.writev(&iovecs[current_iov], batch_size);
        if (result.result == -EAGAIN || result.result == -EWOULDBLOCK) {
            auto poll = co_await stream.poll_write();
            if (poll.result < 0) {
                co_return poll;
            }
            continue;
        }
        if (result.result == -EINTR) {
            continue;
        }
        if (result.result <= 0) {
            co_return result;
        }

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
    
    // Check for overflow before casting to int32_t
    if (total_length > static_cast<size_t>(INT32_MAX)) {
        co_return io::io_result{-EOVERFLOW, 0};
    }
    co_return io::io_result{static_cast<int32_t>(total_length), 0};
}

/// Read a complete frame from the stream, capping the payload at
/// `max_payload`. The cap is checked BEFORE allocating the receive buffer
/// (so a peer cannot trick us into a large allocation), and the underlying
/// connection is closed (caller-side) when the cap is exceeded.
///
/// If the frame has the has_checksum flag set, verifies the CRC32 checksum.
template<rpc_stream Stream>
coro::task<std::optional<std::pair<frame_header, message_buffer>>>
read_frame_bounded(Stream& stream, uint32_t max_payload) {
    // Read header
    std::array<uint8_t, frame_header_size> header_buf;
    auto result = co_await stream.read_exactly(header_buf.data(), frame_header_size);
    if (result.result <= 0) {
        co_return std::nullopt;
    }

    // Parse header
    frame_header header = frame_header::from_bytes(header_buf.data());
    if (!header.is_valid()) {
        ELIO_LOG_ERROR(
            "Invalid frame header: magic={:08x}, version={}, type={}, flags=0x{:02x}, len={}",
            header.magic,
            static_cast<unsigned>(header.version),
            static_cast<unsigned>(header.type),
            static_cast<unsigned>(header.flags),
            header.payload_length);
        co_return std::nullopt;
    }

    // Enforce per-frame payload cap BEFORE allocating, so an attacker
    // sending a 16 MiB header field cannot pre-allocate that much buffer
    // even when the configured cap is much smaller.
    if (header.payload_length > max_payload) {
        ELIO_LOG_ERROR("Frame payload too large: len={} > cap={}",
                       header.payload_length, max_payload);
        co_return std::nullopt;
    }

    // Read payload
    message_buffer payload(header.payload_length);
    if (header.payload_length > 0) {
        result = co_await stream.read_exactly(payload.data(), header.payload_length);
        if (result.result <= 0) {
            co_return std::nullopt;
        }
    }

    // Verify checksum if present (LE on the wire)
    if (has_flag(header.flags, message_flags::has_checksum)) {
        std::array<uint8_t, checksum_size> chk_bytes{};
        result = co_await stream.read_exactly(chk_bytes.data(), checksum_size);
        if (result.result <= 0) {
            co_return std::nullopt;
        }
        uint32_t received_checksum = endian::read_le<uint32_t>(chk_bytes.data());

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

/// Read a complete frame using the protocol-wide max payload cap.
/// Prefer `read_frame_bounded` with an application-specific cap.
template<rpc_stream Stream>
coro::task<std::optional<std::pair<frame_header, message_buffer>>>
read_frame(Stream& stream) {
    return read_frame_bounded(stream, static_cast<uint32_t>(max_message_size));
}

/// Write a complete frame using scatter-gather I/O with partial-write retries
/// If the header has the has_checksum flag set, appends CRC32 checksum
template<rpc_stream Stream>
coro::task<bool> write_frame(Stream& stream, const frame_header& header,
                              const buffer_writer& payload) {
    // Prepare header bytes
    auto header_bytes = header.to_bytes();

    // Compute checksum if needed (must be done before building iovecs)
    std::array<uint8_t, checksum_size> checksum_bytes{};
    if (has_flag(header.flags, message_flags::has_checksum)) {
        uint32_t crc = hash::crc32_update(header_bytes.data(), frame_header_size, 0xFFFFFFFF);
        if (payload.size() > 0) {
            crc = hash::crc32_update(payload.data(), payload.size(), crc);
        }
        endian::write_le<uint32_t>(checksum_bytes.data(), hash::crc32_finalize(crc));
    }

    // Build iovec array for scatter-gather write
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
        iovecs[iov_count].iov_base = checksum_bytes.data();
        iovecs[iov_count].iov_len = checksum_size;
        ++iov_count;
    }

    // Write all data using scatter-gather I/O with retry handling
    auto result = co_await writev_exact(stream, iovecs, iov_count);
    co_return result.result > 0;
}

/// Write a complete frame through a cancellable exact write.
template<cancellable_rpc_stream Stream>
coro::task<bool> write_frame(Stream& stream,
                              const frame_header& header,
                              const buffer_writer& payload,
                              coro::cancel_token token) {
    auto header_bytes = header.to_bytes();

    size_t frame_size = frame_header_size + payload.size();
    std::array<uint8_t, checksum_size> checksum_bytes{};
    if (has_flag(header.flags, message_flags::has_checksum)) {
        frame_size += checksum_size;
        uint32_t crc = hash::crc32_update(header_bytes.data(), frame_header_size, 0xFFFFFFFF);
        if (payload.size() > 0) {
            crc = hash::crc32_update(payload.data(), payload.size(), crc);
        }
        endian::write_le<uint32_t>(checksum_bytes.data(), hash::crc32_finalize(crc));
    }

    buffer_writer frame(frame_size);
    frame.write_bytes(header_bytes.data(), frame_header_size);
    if (payload.size() > 0) {
        frame.write_bytes(payload.data(), payload.size());
    }
    if (has_flag(header.flags, message_flags::has_checksum)) {
        frame.write_bytes(checksum_bytes.data(), checksum_size);
    }

    auto result = co_await stream.write_exactly(frame.data(), frame.size(), token);
    co_return result.result > 0;
}

/// Write a complete frame with raw payload using scatter-gather I/O
/// with partial-write retries
/// If the header has the has_checksum flag set, appends CRC32 checksum
template<rpc_stream Stream>
coro::task<bool> write_frame(Stream& stream, const frame_header& header,
                              const void* payload_data, size_t payload_size) {
    // Prepare header bytes
    auto header_bytes = header.to_bytes();

    // Compute checksum if needed
    std::array<uint8_t, checksum_size> checksum_bytes{};
    if (has_flag(header.flags, message_flags::has_checksum)) {
        uint32_t crc = hash::crc32_update(header_bytes.data(), frame_header_size, 0xFFFFFFFF);
        if (payload_size > 0) {
            crc = hash::crc32_update(payload_data, payload_size, crc);
        }
        endian::write_le<uint32_t>(checksum_bytes.data(), hash::crc32_finalize(crc));
    }

    // Build iovec array for scatter-gather write
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
        iovecs[iov_count].iov_base = checksum_bytes.data();
        iovecs[iov_count].iov_len = checksum_size;
        ++iov_count;
    }

    // Write all data using scatter-gather I/O with retry handling
    auto result = co_await writev_exact(stream, iovecs, iov_count);
    co_return result.result > 0;
}

// ============================================================================
// Message builders
// ============================================================================

/// Validate that payload fits in the wire-format uint32_t length field.
/// The read path already enforces max_message_size (16 MiB); this guards
/// the send path against silent truncation of oversized payloads.
inline void validate_payload_size(size_t size) {
    if (size > UINT32_MAX) {
        throw std::runtime_error("rpc: payload size exceeds uint32_t capacity");
    }
    if (size > max_message_size) {
        throw std::runtime_error("rpc: payload size exceeds max_message_size");
    }
}

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
    validate_payload_size(payload.size());
    header.payload_length = static_cast<uint32_t>(payload.size());
    
    return {header, std::move(payload)};
}

/// Build a one-way request frame. Peers that understand `no_response` must
/// execute the request without emitting response or error frames for this ID.
template<typename Request>
std::pair<frame_header, buffer_writer> build_oneway_request(
    uint32_t request_id,
    method_id_t method_id,
    const Request& request,
    bool enable_checksum = false)
{
    auto frame = build_request(
        request_id, method_id, request, std::nullopt, enable_checksum);
    frame.first.flags = frame.first.flags | message_flags::no_response;
    return frame;
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
    validate_payload_size(payload.size());
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
    validate_payload_size(payload.size());
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
