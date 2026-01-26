#pragma once

/// @file rpc_buffer.hpp
/// @brief Zero-copy buffer management for RPC serialization
///
/// This module provides efficient buffer management for serializing and
/// deserializing RPC messages without memory copies. It supports:
/// - Discontinuous buffers via iovec chains
/// - In-place deserialization with views
/// - Little-endian wire format (host order assumed)
/// - Variable-length data (strings, arrays, blobs)
/// - Buffer references for zero-copy iovec fields

#include <elio/hash/crc32.hpp>

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>
#include <functional>
#include <sys/uio.h>
#include <type_traits>

namespace elio::rpc {

// Import CRC32 functions from hash module for convenience
using elio::hash::crc32;
using elio::hash::crc32_iovec;
using elio::hash::crc32_update;
using elio::hash::crc32_finalize;

/// Maximum message size (16MB)
constexpr size_t max_message_size = 16 * 1024 * 1024;

/// Exception for serialization errors
class serialization_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ============================================================================
// Buffer reference (zero-copy external data reference)
// ============================================================================

/// A reference to external buffer data for zero-copy serialization
/// This type allows including external memory regions (like mmap'd files or 
/// pre-allocated buffers) in RPC messages without copying.
/// 
/// IMPORTANT: The referenced data must remain valid until:
/// - For client: the RPC call completes
/// - For server: the cleanup callback is invoked
class buffer_ref {
public:
    buffer_ref() noexcept = default;
    
    /// Construct from raw pointer and size
    buffer_ref(const void* data, size_t size) noexcept
        : data_(static_cast<const uint8_t*>(data))
        , size_(size) {}
    
    /// Construct from span
    buffer_ref(std::span<const uint8_t> span) noexcept
        : data_(span.data()), size_(span.size()) {}
    
    /// Construct from iovec
    buffer_ref(const struct iovec& iov) noexcept
        : data_(static_cast<const uint8_t*>(iov.iov_base))
        , size_(iov.iov_len) {}
    
    /// Get data pointer
    const uint8_t* data() const noexcept { return data_; }
    
    /// Get size
    size_t size() const noexcept { return size_; }
    
    /// Check if empty
    bool empty() const noexcept { return size_ == 0; }
    
    /// Get as span
    std::span<const uint8_t> span() const noexcept { return {data_, size_}; }
    
    /// Get as iovec
    struct iovec to_iovec() const noexcept {
        return {const_cast<uint8_t*>(data_), size_};
    }
    
    /// Convert to string_view (assumes UTF-8 data)
    std::string_view as_string_view() const noexcept {
        return {reinterpret_cast<const char*>(data_), size_};
    }
    
private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

/// Type trait to detect buffer_ref
template<typename T>
struct is_buffer_ref : std::false_type {};

template<>
struct is_buffer_ref<buffer_ref> : std::true_type {};

template<typename T>
inline constexpr bool is_buffer_ref_v = is_buffer_ref<T>::value;

/// A view into serialized data for zero-copy reading
/// This class provides safe access to serialized fields without copying
class buffer_view {
public:
    buffer_view() noexcept = default;
    
    buffer_view(const void* data, size_t size) noexcept
        : data_(static_cast<const uint8_t*>(data))
        , size_(size) {}
    
    buffer_view(std::span<const uint8_t> span) noexcept
        : data_(span.data()), size_(span.size()) {}
    
    /// Get raw data pointer
    const uint8_t* data() const noexcept { return data_; }
    
    /// Get size in bytes
    size_t size() const noexcept { return size_; }
    
    /// Check if empty
    bool empty() const noexcept { return size_ == 0; }
    
    /// Get remaining bytes from current position
    size_t remaining() const noexcept { return size_ - pos_; }
    
    /// Get current read position
    size_t position() const noexcept { return pos_; }
    
    /// Set read position
    void seek(size_t pos) {
        if (pos > size_) {
            throw serialization_error("seek past end of buffer");
        }
        pos_ = pos;
    }
    
    /// Skip bytes
    void skip(size_t n) {
        if (pos_ + n > size_) {
            throw serialization_error("skip past end of buffer");
        }
        pos_ += n;
    }
    
    /// Read a fixed-size value (little-endian, no conversion needed)
    template<typename T>
    requires std::is_trivially_copyable_v<T>
    T read() {
        if (pos_ + sizeof(T) > size_) {
            throw serialization_error("read past end of buffer");
        }
        T value;
        std::memcpy(&value, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return value;
    }
    
    /// Read into a value (little-endian, no conversion needed)
    template<typename T>
    requires std::is_trivially_copyable_v<T>
    void read_into(T& value) {
        if (pos_ + sizeof(T) > size_) {
            throw serialization_error("read past end of buffer");
        }
        std::memcpy(&value, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
    }
    
    /// Peek at a value without advancing position
    template<typename T>
    requires std::is_trivially_copyable_v<T>
    T peek() const {
        if (pos_ + sizeof(T) > size_) {
            throw serialization_error("peek past end of buffer");
        }
        T value;
        std::memcpy(&value, data_ + pos_, sizeof(T));
        return value;
    }
    
    /// Read raw bytes as a view (zero-copy)
    buffer_view read_bytes(size_t n) {
        if (pos_ + n > size_) {
            throw serialization_error("read_bytes past end of buffer");
        }
        buffer_view result(data_ + pos_, n);
        pos_ += n;
        return result;
    }
    
    /// Read a length-prefixed string view (zero-copy)
    std::string_view read_string() {
        uint32_t len = read<uint32_t>();
        if (pos_ + len > size_) {
            throw serialization_error("string length exceeds buffer");
        }
        std::string_view result(reinterpret_cast<const char*>(data_ + pos_), len);
        pos_ += len;
        return result;
    }
    
    /// Read a length-prefixed byte array as span (zero-copy)
    std::span<const uint8_t> read_blob() {
        uint32_t len = read<uint32_t>();
        if (pos_ + len > size_) {
            throw serialization_error("blob length exceeds buffer");
        }
        std::span<const uint8_t> result(data_ + pos_, len);
        pos_ += len;
        return result;
    }
    
    /// Read array count (for variable-length arrays)
    uint32_t read_array_size() {
        return read<uint32_t>();
    }
    
    /// Get a span of the remaining data
    std::span<const uint8_t> remaining_span() const noexcept {
        return {data_ + pos_, size_ - pos_};
    }
    
    /// Get a span of all data
    std::span<const uint8_t> span() const noexcept {
        return {data_, size_};
    }
    
    /// Convert to string_view (entire buffer)
    std::string_view as_string_view() const noexcept {
        return {reinterpret_cast<const char*>(data_), size_};
    }
    
private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
};

/// A writable buffer for serialization
/// Supports growing and provides access to iovec for scatter I/O
class buffer_writer {
public:
    /// Construct with initial capacity
    explicit buffer_writer(size_t initial_capacity = 256) {
        data_.reserve(initial_capacity);
    }
    
    /// Clear the buffer for reuse
    void clear() noexcept {
        data_.clear();
    }
    
    /// Get current size
    size_t size() const noexcept { return data_.size(); }
    
    /// Get capacity
    size_t capacity() const noexcept { return data_.capacity(); }
    
    /// Check if empty
    bool empty() const noexcept { return data_.empty(); }
    
    /// Reserve capacity
    void reserve(size_t n) { data_.reserve(n); }
    
    /// Get raw data pointer
    const uint8_t* data() const noexcept { return data_.data(); }
    uint8_t* data() noexcept { return data_.data(); }
    
    /// Get as span
    std::span<const uint8_t> span() const noexcept { return data_; }
    std::span<uint8_t> span() noexcept { return data_; }
    
    /// Get as iovec for sendmsg
    struct iovec to_iovec() const noexcept {
        return {const_cast<uint8_t*>(data_.data()), data_.size()};
    }
    
    /// Write a fixed-size value (little-endian, no conversion)
    template<typename T>
    requires std::is_trivially_copyable_v<T>
    void write(T value) {
        size_t old_size = data_.size();
        data_.resize(old_size + sizeof(T));
        std::memcpy(data_.data() + old_size, &value, sizeof(T));
    }
    
    /// Write raw bytes
    void write_bytes(const void* src, size_t n) {
        size_t old_size = data_.size();
        data_.resize(old_size + n);
        std::memcpy(data_.data() + old_size, src, n);
    }
    
    /// Write from span
    void write_bytes(std::span<const uint8_t> src) {
        write_bytes(src.data(), src.size());
    }
    
    /// Write a length-prefixed string
    void write_string(std::string_view str) {
        if (str.size() > UINT32_MAX) {
            throw serialization_error("string too long");
        }
        write(static_cast<uint32_t>(str.size()));
        write_bytes(str.data(), str.size());
    }
    
    /// Write a length-prefixed blob
    void write_blob(std::span<const uint8_t> blob) {
        if (blob.size() > UINT32_MAX) {
            throw serialization_error("blob too long");
        }
        write(static_cast<uint32_t>(blob.size()));
        write_bytes(blob.data(), blob.size());
    }
    
    /// Write array size prefix
    void write_array_size(uint32_t count) {
        write(count);
    }
    
    /// Reserve space and return offset (for back-patching headers)
    size_t reserve_space(size_t n) {
        size_t offset = data_.size();
        data_.resize(offset + n);
        return offset;
    }
    
    /// Write at a specific offset (for back-patching)
    template<typename T>
    requires std::is_trivially_copyable_v<T>
    void write_at(size_t offset, T value) {
        if (offset + sizeof(T) > data_.size()) {
            throw serialization_error("write_at past end of buffer");
        }
        std::memcpy(data_.data() + offset, &value, sizeof(T));
    }
    
    /// Get buffer view for reading what was written
    buffer_view view() const noexcept {
        return buffer_view(data_.data(), data_.size());
    }
    
    /// Move the internal buffer out
    std::vector<uint8_t> release() noexcept {
        return std::move(data_);
    }
    
private:
    std::vector<uint8_t> data_;
};

/// Discontinuous buffer for scatter-gather I/O
/// Allows building messages from multiple non-contiguous memory regions
class iovec_buffer {
public:
    /// Add a buffer segment
    void add(const void* data, size_t size) {
        if (size > 0) {
            iovecs_.push_back({const_cast<void*>(data), size});
            total_size_ += size;
        }
    }
    
    /// Add from span
    void add(std::span<const uint8_t> span) {
        add(span.data(), span.size());
    }
    
    /// Add from buffer_writer
    void add(const buffer_writer& writer) {
        add(writer.data(), writer.size());
    }
    
    /// Add from string_view
    void add(std::string_view str) {
        add(str.data(), str.size());
    }
    
    /// Clear all segments
    void clear() noexcept {
        iovecs_.clear();
        total_size_ = 0;
    }
    
    /// Get iovec array for sendmsg/writev
    struct iovec* iovecs() noexcept { return iovecs_.data(); }
    const struct iovec* iovecs() const noexcept { return iovecs_.data(); }
    
    /// Get number of iovec entries
    size_t count() const noexcept { return iovecs_.size(); }
    
    /// Get total size across all segments
    size_t total_size() const noexcept { return total_size_; }
    
    /// Check if empty
    bool empty() const noexcept { return iovecs_.empty(); }
    
    /// Flatten into a single contiguous buffer (copies data)
    std::vector<uint8_t> flatten() const {
        std::vector<uint8_t> result;
        result.reserve(total_size_);
        for (const auto& iov : iovecs_) {
            const uint8_t* ptr = static_cast<const uint8_t*>(iov.iov_base);
            result.insert(result.end(), ptr, ptr + iov.iov_len);
        }
        return result;
    }
    
private:
    std::vector<struct iovec> iovecs_;
    size_t total_size_ = 0;
};

/// A received message buffer that owns its data
/// Used for receiving complete RPC messages
class message_buffer {
public:
    message_buffer() = default;
    
    explicit message_buffer(size_t size) : data_(size) {}
    
    explicit message_buffer(std::vector<uint8_t> data) 
        : data_(std::move(data)) {}
    
    /// Get writable data pointer (for receiving)
    uint8_t* data() noexcept { return data_.data(); }
    const uint8_t* data() const noexcept { return data_.data(); }
    
    /// Get size
    size_t size() const noexcept { return data_.size(); }
    
    /// Check if empty
    bool empty() const noexcept { return data_.empty(); }
    
    /// Resize buffer
    void resize(size_t n) { data_.resize(n); }
    
    /// Reserve capacity
    void reserve(size_t n) { data_.reserve(n); }
    
    /// Clear buffer
    void clear() noexcept { data_.clear(); }
    
    /// Get a view for reading
    buffer_view view() const noexcept {
        return buffer_view(data_.data(), data_.size());
    }
    
    /// Get as span
    std::span<uint8_t> span() noexcept { return data_; }
    std::span<const uint8_t> span() const noexcept { return data_; }
    
    /// Get as iovec for recvmsg
    struct iovec to_iovec() noexcept {
        return {data_.data(), data_.size()};
    }
    
    /// Move the internal buffer out
    std::vector<uint8_t> release() noexcept {
        return std::move(data_);
    }
    
private:
    std::vector<uint8_t> data_;
};

} // namespace elio::rpc
