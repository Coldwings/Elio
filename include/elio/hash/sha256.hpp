#pragma once

/// @file sha256.hpp
/// @brief Optimized SHA-256 hash implementation
///
/// Provides SHA-256 cryptographic hash computation with:
/// - Loop unrolling for better instruction-level parallelism
/// - Aligned memory access optimization
/// - Efficient message schedule computation
///
/// SHA-256 is part of the SHA-2 family and provides strong security.

#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace elio::hash {

/// SHA-256 digest size in bytes (256 bits)
constexpr size_t sha256_digest_size = 32;

/// SHA-256 block size in bytes
constexpr size_t sha256_block_size = 64;

/// SHA-256 digest type
using sha256_digest = std::array<uint8_t, sha256_digest_size>;

// ============================================================================
// SHA-256 constants
// ============================================================================

namespace detail {

/// SHA-256 round constants (first 32 bits of fractional parts of cube roots of first 64 primes)
constexpr uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

} // namespace detail

// ============================================================================
// SHA-256 context for incremental hashing
// ============================================================================

/// SHA-256 hash context for incremental computation
class sha256_context {
public:
    sha256_context() noexcept { reset(); }
    
    /// Reset context to initial state
    void reset() noexcept {
        // Initial hash values (first 32 bits of fractional parts of square roots of first 8 primes)
        state_[0] = 0x6a09e667;
        state_[1] = 0xbb67ae85;
        state_[2] = 0x3c6ef372;
        state_[3] = 0xa54ff53a;
        state_[4] = 0x510e527f;
        state_[5] = 0x9b05688c;
        state_[6] = 0x1f83d9ab;
        state_[7] = 0x5be0cd19;
        count_ = 0;
        buffer_len_ = 0;
    }
    
    /// Update hash with more data
    void update(const void* data, size_t length) noexcept {
        const uint8_t* input = static_cast<const uint8_t*>(data);
        count_ += length;
        
        // Process any data remaining in buffer
        if (buffer_len_ > 0) {
            size_t to_copy = std::min(sha256_block_size - buffer_len_, length);
            std::memcpy(buffer_ + buffer_len_, input, to_copy);
            buffer_len_ += to_copy;
            input += to_copy;
            length -= to_copy;
            
            if (buffer_len_ == sha256_block_size) {
                process_block(buffer_);
                buffer_len_ = 0;
            }
        }
        
        // Process full blocks directly from input
        while (length >= sha256_block_size) {
            process_block(input);
            input += sha256_block_size;
            length -= sha256_block_size;
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
    sha256_digest finalize() noexcept {
        // Save the message length before padding
        uint64_t total_bits = count_ * 8;
        
        // Pad message: append 1 bit, then zeros, then 64-bit length
        // We need the final length to be 56 mod 64 (leaving 8 bytes for length)
        size_t pad_len = (buffer_len_ < 56) ? (56 - buffer_len_) : (120 - buffer_len_);
        
        alignas(8) uint8_t padding[128];
        padding[0] = 0x80;
        std::memset(padding + 1, 0, pad_len - 1);
        
        // Append length in bits (big-endian) - don't count these bytes
        for (int i = 0; i < 8; ++i) {
            padding[pad_len + i] = static_cast<uint8_t>(total_bits >> (56 - i * 8));
        }
        
        // Process padding + length without updating count_
        const uint8_t* input = padding;
        size_t remaining = pad_len + 8;
        
        // Fill buffer and process
        if (buffer_len_ > 0) {
            size_t to_copy = std::min(sha256_block_size - buffer_len_, remaining);
            std::memcpy(buffer_ + buffer_len_, input, to_copy);
            buffer_len_ += to_copy;
            input += to_copy;
            remaining -= to_copy;
            
            if (buffer_len_ == sha256_block_size) {
                process_block(buffer_);
                buffer_len_ = 0;
            }
        }
        
        while (remaining >= sha256_block_size) {
            process_block(input);
            input += sha256_block_size;
            remaining -= sha256_block_size;
        }
        
        // Extract digest (big-endian)
        sha256_digest digest;
        for (int i = 0; i < 8; ++i) {
            digest[i * 4 + 0] = static_cast<uint8_t>(state_[i] >> 24);
            digest[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
            digest[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
            digest[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
        }
        
        return digest;
    }
    
private:
    static uint32_t rotr(uint32_t x, int n) noexcept {
        return (x >> n) | (x << (32 - n));
    }
    
    static uint32_t load_be32(const uint8_t* p) noexcept {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               (static_cast<uint32_t>(p[3]));
    }
    
    // SHA-256 helper functions
    static uint32_t ch(uint32_t e, uint32_t f, uint32_t g) noexcept {
        return (e & f) ^ ((~e) & g);
    }
    
    static uint32_t maj(uint32_t a, uint32_t b, uint32_t c) noexcept {
        return (a & b) ^ (a & c) ^ (b & c);
    }
    
    static uint32_t sigma0(uint32_t a) noexcept {
        return rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    }
    
    static uint32_t sigma1(uint32_t e) noexcept {
        return rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    }
    
    // Unrolled round macro
    #define SHA256_ROUND(a, b, c, d, e, f, g, h, i) \
        do { \
            uint32_t t1 = h + sigma1(e) + ch(e, f, g) + detail::sha256_k[i] + w[i]; \
            uint32_t t2 = sigma0(a) + maj(a, b, c); \
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2; \
        } while(0)
    
    void process_block(const uint8_t* block) noexcept {
        alignas(16) uint32_t w[64];
        
        // Load block into w[0..15] (big-endian)
        for (int i = 0; i < 16; ++i) {
            w[i] = load_be32(block + i * 4);
        }
        
        // Extend to w[16..63] - unrolled in groups of 8
        #define SHA256_SCHEDULE(i) \
            do { \
                uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3); \
                uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10); \
                w[i] = w[i-16] + s0 + w[i-7] + s1; \
            } while(0)
        
        SHA256_SCHEDULE(16); SHA256_SCHEDULE(17); SHA256_SCHEDULE(18); SHA256_SCHEDULE(19);
        SHA256_SCHEDULE(20); SHA256_SCHEDULE(21); SHA256_SCHEDULE(22); SHA256_SCHEDULE(23);
        SHA256_SCHEDULE(24); SHA256_SCHEDULE(25); SHA256_SCHEDULE(26); SHA256_SCHEDULE(27);
        SHA256_SCHEDULE(28); SHA256_SCHEDULE(29); SHA256_SCHEDULE(30); SHA256_SCHEDULE(31);
        SHA256_SCHEDULE(32); SHA256_SCHEDULE(33); SHA256_SCHEDULE(34); SHA256_SCHEDULE(35);
        SHA256_SCHEDULE(36); SHA256_SCHEDULE(37); SHA256_SCHEDULE(38); SHA256_SCHEDULE(39);
        SHA256_SCHEDULE(40); SHA256_SCHEDULE(41); SHA256_SCHEDULE(42); SHA256_SCHEDULE(43);
        SHA256_SCHEDULE(44); SHA256_SCHEDULE(45); SHA256_SCHEDULE(46); SHA256_SCHEDULE(47);
        SHA256_SCHEDULE(48); SHA256_SCHEDULE(49); SHA256_SCHEDULE(50); SHA256_SCHEDULE(51);
        SHA256_SCHEDULE(52); SHA256_SCHEDULE(53); SHA256_SCHEDULE(54); SHA256_SCHEDULE(55);
        SHA256_SCHEDULE(56); SHA256_SCHEDULE(57); SHA256_SCHEDULE(58); SHA256_SCHEDULE(59);
        SHA256_SCHEDULE(60); SHA256_SCHEDULE(61); SHA256_SCHEDULE(62); SHA256_SCHEDULE(63);
        
        #undef SHA256_SCHEDULE
        
        uint32_t a = state_[0];
        uint32_t b = state_[1];
        uint32_t c = state_[2];
        uint32_t d = state_[3];
        uint32_t e = state_[4];
        uint32_t f = state_[5];
        uint32_t g = state_[6];
        uint32_t h = state_[7];
        
        // Rounds 0-63 (fully unrolled)
        SHA256_ROUND(a, b, c, d, e, f, g, h,  0); SHA256_ROUND(a, b, c, d, e, f, g, h,  1);
        SHA256_ROUND(a, b, c, d, e, f, g, h,  2); SHA256_ROUND(a, b, c, d, e, f, g, h,  3);
        SHA256_ROUND(a, b, c, d, e, f, g, h,  4); SHA256_ROUND(a, b, c, d, e, f, g, h,  5);
        SHA256_ROUND(a, b, c, d, e, f, g, h,  6); SHA256_ROUND(a, b, c, d, e, f, g, h,  7);
        SHA256_ROUND(a, b, c, d, e, f, g, h,  8); SHA256_ROUND(a, b, c, d, e, f, g, h,  9);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 10); SHA256_ROUND(a, b, c, d, e, f, g, h, 11);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 12); SHA256_ROUND(a, b, c, d, e, f, g, h, 13);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 14); SHA256_ROUND(a, b, c, d, e, f, g, h, 15);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 16); SHA256_ROUND(a, b, c, d, e, f, g, h, 17);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 18); SHA256_ROUND(a, b, c, d, e, f, g, h, 19);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 20); SHA256_ROUND(a, b, c, d, e, f, g, h, 21);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 22); SHA256_ROUND(a, b, c, d, e, f, g, h, 23);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 24); SHA256_ROUND(a, b, c, d, e, f, g, h, 25);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 26); SHA256_ROUND(a, b, c, d, e, f, g, h, 27);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 28); SHA256_ROUND(a, b, c, d, e, f, g, h, 29);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 30); SHA256_ROUND(a, b, c, d, e, f, g, h, 31);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 32); SHA256_ROUND(a, b, c, d, e, f, g, h, 33);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 34); SHA256_ROUND(a, b, c, d, e, f, g, h, 35);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 36); SHA256_ROUND(a, b, c, d, e, f, g, h, 37);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 38); SHA256_ROUND(a, b, c, d, e, f, g, h, 39);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 40); SHA256_ROUND(a, b, c, d, e, f, g, h, 41);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 42); SHA256_ROUND(a, b, c, d, e, f, g, h, 43);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 44); SHA256_ROUND(a, b, c, d, e, f, g, h, 45);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 46); SHA256_ROUND(a, b, c, d, e, f, g, h, 47);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 48); SHA256_ROUND(a, b, c, d, e, f, g, h, 49);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 50); SHA256_ROUND(a, b, c, d, e, f, g, h, 51);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 52); SHA256_ROUND(a, b, c, d, e, f, g, h, 53);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 54); SHA256_ROUND(a, b, c, d, e, f, g, h, 55);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 56); SHA256_ROUND(a, b, c, d, e, f, g, h, 57);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 58); SHA256_ROUND(a, b, c, d, e, f, g, h, 59);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 60); SHA256_ROUND(a, b, c, d, e, f, g, h, 61);
        SHA256_ROUND(a, b, c, d, e, f, g, h, 62); SHA256_ROUND(a, b, c, d, e, f, g, h, 63);
        
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }
    
    #undef SHA256_ROUND
    
    alignas(16) uint32_t state_[8];
    uint64_t count_;
    alignas(16) uint8_t buffer_[sha256_block_size];
    size_t buffer_len_;
};

// ============================================================================
// Convenience functions
// ============================================================================

/// Compute SHA-256 hash of a buffer
inline sha256_digest sha256(const void* data, size_t length) {
    sha256_context ctx;
    ctx.update(data, length);
    return ctx.finalize();
}

/// Compute SHA-256 hash of a span
inline sha256_digest sha256(std::span<const uint8_t> data) {
    return sha256(data.data(), data.size());
}

/// Compute SHA-256 hash of a string
inline sha256_digest sha256(std::string_view str) {
    return sha256(str.data(), str.size());
}

/// Convert SHA-256 digest to hexadecimal string
inline std::string sha256_hex(const sha256_digest& digest) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(sha256_digest_size * 2);
    for (uint8_t byte : digest) {
        result += hex_chars[(byte >> 4) & 0x0F];
        result += hex_chars[byte & 0x0F];
    }
    return result;
}

/// Compute SHA-256 and return as hex string
inline std::string sha256_hex(const void* data, size_t length) {
    return sha256_hex(sha256(data, length));
}

/// Compute SHA-256 and return as hex string
inline std::string sha256_hex(std::string_view str) {
    return sha256_hex(sha256(str));
}

} // namespace elio::hash
