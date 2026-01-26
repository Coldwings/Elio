#pragma once

/// @file sha1.hpp
/// @brief Optimized SHA-1 hash implementation
///
/// Provides SHA-1 cryptographic hash computation with:
/// - Loop unrolling for better instruction-level parallelism
/// - Aligned memory access optimization
/// - Efficient message schedule computation
///
/// Note: SHA-1 is considered weak for cryptographic purposes.
/// Use SHA-256 or stronger for security-sensitive applications.

#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace elio::hash {

/// SHA-1 digest size in bytes (160 bits)
constexpr size_t sha1_digest_size = 20;

/// SHA-1 block size in bytes
constexpr size_t sha1_block_size = 64;

/// SHA-1 digest type
using sha1_digest = std::array<uint8_t, sha1_digest_size>;

// ============================================================================
// SHA-1 context for incremental hashing
// ============================================================================

/// SHA-1 hash context for incremental computation
class sha1_context {
public:
    sha1_context() noexcept { reset(); }
    
    /// Reset context to initial state
    void reset() noexcept {
        state_[0] = 0x67452301;
        state_[1] = 0xEFCDAB89;
        state_[2] = 0x98BADCFE;
        state_[3] = 0x10325476;
        state_[4] = 0xC3D2E1F0;
        count_ = 0;
        buffer_len_ = 0;
    }
    
    /// Update hash with more data
    void update(const void* data, size_t length) noexcept {
        const uint8_t* input = static_cast<const uint8_t*>(data);
        count_ += length;
        
        // Process any data remaining in buffer
        if (buffer_len_ > 0) {
            size_t to_copy = sha1_block_size - buffer_len_;
            if (to_copy > length) to_copy = length;
            std::memcpy(buffer_ + buffer_len_, input, to_copy);
            buffer_len_ += to_copy;
            input += to_copy;
            length -= to_copy;
            
            if (buffer_len_ == sha1_block_size) {
                process_block(buffer_);
                buffer_len_ = 0;
            }
        }
        
        // Process full blocks directly from input
        while (length >= sha1_block_size) {
            process_block(input);
            input += sha1_block_size;
            length -= sha1_block_size;
        }
        
        // Buffer remaining data
        if (length > 0) {
            std::memcpy(buffer_, input, length);
            buffer_len_ = length;
        }
    }
    
    /// Update hash with span
    void update(std::span<const uint8_t> data) noexcept {
        update(data.data(), data.size());
    }
    
    /// Update hash with string
    void update(std::string_view str) noexcept {
        update(str.data(), str.size());
    }
    
    /// Finalize and return digest
    sha1_digest finalize() noexcept {
        // Save the message length before padding
        uint64_t total_bits = count_ * 8;
        
        // Calculate padding length
        size_t pad_len = (buffer_len_ < 56) ? (56 - buffer_len_) : (120 - buffer_len_);
        
        // Prepare padding + length block
        alignas(8) uint8_t padding[128];
        padding[0] = 0x80;
        std::memset(padding + 1, 0, pad_len - 1);
        
        // Append length in bits (big-endian)
        for (int i = 0; i < 8; ++i) {
            padding[pad_len + i] = static_cast<uint8_t>(total_bits >> (56 - i * 8));
        }
        
        // Process padding + length
        const uint8_t* input = padding;
        size_t remaining = pad_len + 8;
        
        if (buffer_len_ > 0) {
            size_t to_copy = sha1_block_size - buffer_len_;
            if (to_copy > remaining) to_copy = remaining;
            std::memcpy(buffer_ + buffer_len_, input, to_copy);
            buffer_len_ += to_copy;
            input += to_copy;
            remaining -= to_copy;
            
            if (buffer_len_ == sha1_block_size) {
                process_block(buffer_);
                buffer_len_ = 0;
            }
        }
        
        while (remaining >= sha1_block_size) {
            process_block(input);
            input += sha1_block_size;
            remaining -= sha1_block_size;
        }
        
        // Extract digest (big-endian)
        sha1_digest digest;
        for (int i = 0; i < 5; ++i) {
            digest[i * 4 + 0] = static_cast<uint8_t>(state_[i] >> 24);
            digest[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
            digest[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
            digest[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
        }
        
        return digest;
    }
    
private:
    static uint32_t rotl(uint32_t x, int n) noexcept {
        return (x << n) | (x >> (32 - n));
    }
    
    static uint32_t load_be32(const uint8_t* p) noexcept {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               (static_cast<uint32_t>(p[3]));
    }
    
    // SHA-1 round functions
    static uint32_t f0(uint32_t b, uint32_t c, uint32_t d) noexcept {
        return (b & c) | ((~b) & d);
    }
    
    static uint32_t f1(uint32_t b, uint32_t c, uint32_t d) noexcept {
        return b ^ c ^ d;
    }
    
    static uint32_t f2(uint32_t b, uint32_t c, uint32_t d) noexcept {
        return (b & c) | (b & d) | (c & d);
    }
    
    // Unrolled round macros
    #define SHA1_ROUND0(a, b, c, d, e, i) \
        e += rotl(a, 5) + f0(b, c, d) + 0x5A827999 + w[i]; \
        b = rotl(b, 30)
    
    #define SHA1_ROUND1(a, b, c, d, e, i) \
        e += rotl(a, 5) + f1(b, c, d) + 0x6ED9EBA1 + w[i]; \
        b = rotl(b, 30)
    
    #define SHA1_ROUND2(a, b, c, d, e, i) \
        e += rotl(a, 5) + f2(b, c, d) + 0x8F1BBCDC + w[i]; \
        b = rotl(b, 30)
    
    #define SHA1_ROUND3(a, b, c, d, e, i) \
        e += rotl(a, 5) + f1(b, c, d) + 0xCA62C1D6 + w[i]; \
        b = rotl(b, 30)
    
    void process_block(const uint8_t* block) noexcept {
        uint32_t w[80];
        
        // Load block into w[0..15] (big-endian)
        for (int i = 0; i < 16; ++i) {
            w[i] = load_be32(block + i * 4);
        }
        
        // Extend to w[16..79]
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        }
        
        uint32_t a = state_[0];
        uint32_t b = state_[1];
        uint32_t c = state_[2];
        uint32_t d = state_[3];
        uint32_t e = state_[4];
        
        // Rounds 0-19 (unrolled by 5)
        SHA1_ROUND0(a, b, c, d, e,  0); SHA1_ROUND0(e, a, b, c, d,  1);
        SHA1_ROUND0(d, e, a, b, c,  2); SHA1_ROUND0(c, d, e, a, b,  3);
        SHA1_ROUND0(b, c, d, e, a,  4); SHA1_ROUND0(a, b, c, d, e,  5);
        SHA1_ROUND0(e, a, b, c, d,  6); SHA1_ROUND0(d, e, a, b, c,  7);
        SHA1_ROUND0(c, d, e, a, b,  8); SHA1_ROUND0(b, c, d, e, a,  9);
        SHA1_ROUND0(a, b, c, d, e, 10); SHA1_ROUND0(e, a, b, c, d, 11);
        SHA1_ROUND0(d, e, a, b, c, 12); SHA1_ROUND0(c, d, e, a, b, 13);
        SHA1_ROUND0(b, c, d, e, a, 14); SHA1_ROUND0(a, b, c, d, e, 15);
        SHA1_ROUND0(e, a, b, c, d, 16); SHA1_ROUND0(d, e, a, b, c, 17);
        SHA1_ROUND0(c, d, e, a, b, 18); SHA1_ROUND0(b, c, d, e, a, 19);
        
        // Rounds 20-39
        SHA1_ROUND1(a, b, c, d, e, 20); SHA1_ROUND1(e, a, b, c, d, 21);
        SHA1_ROUND1(d, e, a, b, c, 22); SHA1_ROUND1(c, d, e, a, b, 23);
        SHA1_ROUND1(b, c, d, e, a, 24); SHA1_ROUND1(a, b, c, d, e, 25);
        SHA1_ROUND1(e, a, b, c, d, 26); SHA1_ROUND1(d, e, a, b, c, 27);
        SHA1_ROUND1(c, d, e, a, b, 28); SHA1_ROUND1(b, c, d, e, a, 29);
        SHA1_ROUND1(a, b, c, d, e, 30); SHA1_ROUND1(e, a, b, c, d, 31);
        SHA1_ROUND1(d, e, a, b, c, 32); SHA1_ROUND1(c, d, e, a, b, 33);
        SHA1_ROUND1(b, c, d, e, a, 34); SHA1_ROUND1(a, b, c, d, e, 35);
        SHA1_ROUND1(e, a, b, c, d, 36); SHA1_ROUND1(d, e, a, b, c, 37);
        SHA1_ROUND1(c, d, e, a, b, 38); SHA1_ROUND1(b, c, d, e, a, 39);
        
        // Rounds 40-59
        SHA1_ROUND2(a, b, c, d, e, 40); SHA1_ROUND2(e, a, b, c, d, 41);
        SHA1_ROUND2(d, e, a, b, c, 42); SHA1_ROUND2(c, d, e, a, b, 43);
        SHA1_ROUND2(b, c, d, e, a, 44); SHA1_ROUND2(a, b, c, d, e, 45);
        SHA1_ROUND2(e, a, b, c, d, 46); SHA1_ROUND2(d, e, a, b, c, 47);
        SHA1_ROUND2(c, d, e, a, b, 48); SHA1_ROUND2(b, c, d, e, a, 49);
        SHA1_ROUND2(a, b, c, d, e, 50); SHA1_ROUND2(e, a, b, c, d, 51);
        SHA1_ROUND2(d, e, a, b, c, 52); SHA1_ROUND2(c, d, e, a, b, 53);
        SHA1_ROUND2(b, c, d, e, a, 54); SHA1_ROUND2(a, b, c, d, e, 55);
        SHA1_ROUND2(e, a, b, c, d, 56); SHA1_ROUND2(d, e, a, b, c, 57);
        SHA1_ROUND2(c, d, e, a, b, 58); SHA1_ROUND2(b, c, d, e, a, 59);
        
        // Rounds 60-79
        SHA1_ROUND3(a, b, c, d, e, 60); SHA1_ROUND3(e, a, b, c, d, 61);
        SHA1_ROUND3(d, e, a, b, c, 62); SHA1_ROUND3(c, d, e, a, b, 63);
        SHA1_ROUND3(b, c, d, e, a, 64); SHA1_ROUND3(a, b, c, d, e, 65);
        SHA1_ROUND3(e, a, b, c, d, 66); SHA1_ROUND3(d, e, a, b, c, 67);
        SHA1_ROUND3(c, d, e, a, b, 68); SHA1_ROUND3(b, c, d, e, a, 69);
        SHA1_ROUND3(a, b, c, d, e, 70); SHA1_ROUND3(e, a, b, c, d, 71);
        SHA1_ROUND3(d, e, a, b, c, 72); SHA1_ROUND3(c, d, e, a, b, 73);
        SHA1_ROUND3(b, c, d, e, a, 74); SHA1_ROUND3(a, b, c, d, e, 75);
        SHA1_ROUND3(e, a, b, c, d, 76); SHA1_ROUND3(d, e, a, b, c, 77);
        SHA1_ROUND3(c, d, e, a, b, 78); SHA1_ROUND3(b, c, d, e, a, 79);
        
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
    }
    
    #undef SHA1_ROUND0
    #undef SHA1_ROUND1
    #undef SHA1_ROUND2
    #undef SHA1_ROUND3
    
    alignas(16) uint32_t state_[5];
    uint64_t count_;
    alignas(16) uint8_t buffer_[sha1_block_size];
    size_t buffer_len_;
};

// ============================================================================
// Convenience functions
// ============================================================================

/// Compute SHA-1 hash of a buffer
inline sha1_digest sha1(const void* data, size_t length) {
    sha1_context ctx;
    ctx.update(data, length);
    return ctx.finalize();
}

/// Compute SHA-1 hash of a span
inline sha1_digest sha1(std::span<const uint8_t> data) {
    return sha1(data.data(), data.size());
}

/// Compute SHA-1 hash of a string
inline sha1_digest sha1(std::string_view str) {
    return sha1(str.data(), str.size());
}

/// Convert SHA-1 digest to hexadecimal string
inline std::string sha1_hex(const sha1_digest& digest) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(sha1_digest_size * 2);
    for (uint8_t byte : digest) {
        result += hex_chars[(byte >> 4) & 0x0F];
        result += hex_chars[byte & 0x0F];
    }
    return result;
}

/// Compute SHA-1 and return as hex string
inline std::string sha1_hex(const void* data, size_t length) {
    return sha1_hex(sha1(data, length));
}

/// Compute SHA-1 and return as hex string
inline std::string sha1_hex(std::string_view str) {
    return sha1_hex(sha1(str));
}

} // namespace elio::hash
