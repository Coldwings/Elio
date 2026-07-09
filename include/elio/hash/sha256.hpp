#pragma once

/// @file sha256.hpp
/// @brief Optimized SHA-256 hash implementation
///
/// Provides SHA-256 cryptographic hash computation with:
/// - Loop unrolling for better instruction-level parallelism
/// - Aligned memory access optimization
/// - Efficient message schedule computation
/// - Optional hardware acceleration on x86 (SHA-NI) and ARMv8 (SHA2 ext)
///
/// The hardware fast path is selected at runtime via `cpu_features` and
/// produces digests bit-for-bit identical to the software path. Use
/// `set_force_software_hash(true)` from `<elio/hash/cpu_features.hpp>` to
/// pin all hashing onto the software path (e.g. for unit-test parity).
///
/// SHA-256 is part of the SHA-2 family and provides strong security.

#include "cpu_features.hpp"

#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#if defined(__aarch64__) || defined(_M_ARM64)
#define ELIO_SHA256_HAVE_ARM_DISPATCH 1
#include <arm_neon.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define ELIO_SHA256_HAVE_X86_DISPATCH 1
#include <immintrin.h>
#endif

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
#ifdef ELIO_SHA256_HAVE_ARM_DISPATCH
        if (use_arm_sha2()) {
            process_block_arm(block);
            return;
        }
#endif
#ifdef ELIO_SHA256_HAVE_X86_DISPATCH
        if (use_sha_ni()) {
            process_block_sha_ni(block);
            return;
        }
#endif
        process_block_sw(block);
    }

    void process_block_sw(const uint8_t* block) noexcept {
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

#ifdef ELIO_SHA256_HAVE_ARM_DISPATCH
    void process_block_arm(const uint8_t* block) noexcept;
#endif
#ifdef ELIO_SHA256_HAVE_X86_DISPATCH
    void process_block_sha_ni(const uint8_t* block) noexcept;
#endif

    alignas(16) uint32_t state_[8];
    uint64_t count_;
    alignas(16) uint8_t buffer_[sha256_block_size];
    size_t buffer_len_;
};

// ============================================================================
// Hardware-accelerated block processors (out-of-class definitions)
// ============================================================================

#ifdef ELIO_SHA256_HAVE_ARM_DISPATCH
#pragma GCC push_options
#pragma GCC target("+crypto")

inline void sha256_context::process_block_arm(const uint8_t* block) noexcept {
    // Reference: Crypto++ sha_simd.cpp SHA256_HashMultipleBlocks_ARMV8.
    // STATE0 holds A..D, STATE1 holds E..H.
    uint32x4_t STATE0 = vld1q_u32(&state_[0]);
    uint32x4_t STATE1 = vld1q_u32(&state_[4]);

    const uint32x4_t ABEF_SAVE = STATE0;
    const uint32x4_t CDGH_SAVE = STATE1;

    uint32x4_t MSG0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 0)));
    uint32x4_t MSG1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 16)));
    uint32x4_t MSG2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 32)));
    uint32x4_t MSG3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 48)));

    uint32x4_t TMP0, TMP1, TMP2;

    TMP0 = vaddq_u32(MSG0, vld1q_u32(&detail::sha256_k[0x00]));

    // Rounds 0-3
    MSG0 = vsha256su0q_u32(MSG0, MSG1);
    TMP2 = STATE0;
    TMP1 = vaddq_u32(MSG1, vld1q_u32(&detail::sha256_k[0x04]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
    MSG0 = vsha256su1q_u32(MSG0, MSG2, MSG3);

    // Rounds 4-7
    MSG1 = vsha256su0q_u32(MSG1, MSG2);
    TMP2 = STATE0;
    TMP0 = vaddq_u32(MSG2, vld1q_u32(&detail::sha256_k[0x08]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
    MSG1 = vsha256su1q_u32(MSG1, MSG3, MSG0);

    // Rounds 8-11
    MSG2 = vsha256su0q_u32(MSG2, MSG3);
    TMP2 = STATE0;
    TMP1 = vaddq_u32(MSG3, vld1q_u32(&detail::sha256_k[0x0c]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
    MSG2 = vsha256su1q_u32(MSG2, MSG0, MSG1);

    // Rounds 12-15
    MSG3 = vsha256su0q_u32(MSG3, MSG0);
    TMP2 = STATE0;
    TMP0 = vaddq_u32(MSG0, vld1q_u32(&detail::sha256_k[0x10]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
    MSG3 = vsha256su1q_u32(MSG3, MSG1, MSG2);

    // Rounds 16-19
    MSG0 = vsha256su0q_u32(MSG0, MSG1);
    TMP2 = STATE0;
    TMP1 = vaddq_u32(MSG1, vld1q_u32(&detail::sha256_k[0x14]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
    MSG0 = vsha256su1q_u32(MSG0, MSG2, MSG3);

    // Rounds 20-23
    MSG1 = vsha256su0q_u32(MSG1, MSG2);
    TMP2 = STATE0;
    TMP0 = vaddq_u32(MSG2, vld1q_u32(&detail::sha256_k[0x18]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
    MSG1 = vsha256su1q_u32(MSG1, MSG3, MSG0);

    // Rounds 24-27
    MSG2 = vsha256su0q_u32(MSG2, MSG3);
    TMP2 = STATE0;
    TMP1 = vaddq_u32(MSG3, vld1q_u32(&detail::sha256_k[0x1c]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
    MSG2 = vsha256su1q_u32(MSG2, MSG0, MSG1);

    // Rounds 28-31
    MSG3 = vsha256su0q_u32(MSG3, MSG0);
    TMP2 = STATE0;
    TMP0 = vaddq_u32(MSG0, vld1q_u32(&detail::sha256_k[0x20]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
    MSG3 = vsha256su1q_u32(MSG3, MSG1, MSG2);

    // Rounds 32-35
    MSG0 = vsha256su0q_u32(MSG0, MSG1);
    TMP2 = STATE0;
    TMP1 = vaddq_u32(MSG1, vld1q_u32(&detail::sha256_k[0x24]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
    MSG0 = vsha256su1q_u32(MSG0, MSG2, MSG3);

    // Rounds 36-39
    MSG1 = vsha256su0q_u32(MSG1, MSG2);
    TMP2 = STATE0;
    TMP0 = vaddq_u32(MSG2, vld1q_u32(&detail::sha256_k[0x28]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
    MSG1 = vsha256su1q_u32(MSG1, MSG3, MSG0);

    // Rounds 40-43
    MSG2 = vsha256su0q_u32(MSG2, MSG3);
    TMP2 = STATE0;
    TMP1 = vaddq_u32(MSG3, vld1q_u32(&detail::sha256_k[0x2c]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
    MSG2 = vsha256su1q_u32(MSG2, MSG0, MSG1);

    // Rounds 44-47
    MSG3 = vsha256su0q_u32(MSG3, MSG0);
    TMP2 = STATE0;
    TMP0 = vaddq_u32(MSG0, vld1q_u32(&detail::sha256_k[0x30]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
    MSG3 = vsha256su1q_u32(MSG3, MSG1, MSG2);

    // Rounds 48-51
    TMP2 = STATE0;
    TMP1 = vaddq_u32(MSG1, vld1q_u32(&detail::sha256_k[0x34]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);

    // Rounds 52-55
    TMP2 = STATE0;
    TMP0 = vaddq_u32(MSG2, vld1q_u32(&detail::sha256_k[0x38]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);

    // Rounds 56-59
    TMP2 = STATE0;
    TMP1 = vaddq_u32(MSG3, vld1q_u32(&detail::sha256_k[0x3c]));
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);

    // Rounds 60-63
    TMP2 = STATE0;
    STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
    STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);

    STATE0 = vaddq_u32(STATE0, ABEF_SAVE);
    STATE1 = vaddq_u32(STATE1, CDGH_SAVE);

    vst1q_u32(&state_[0], STATE0);
    vst1q_u32(&state_[4], STATE1);
}

#pragma GCC pop_options
#endif // ELIO_SHA256_HAVE_ARM_DISPATCH

#ifdef ELIO_SHA256_HAVE_X86_DISPATCH
#pragma GCC push_options
#pragma GCC target("sha,sse4.2,ssse3")

inline void sha256_context::process_block_sha_ni(const uint8_t* block) noexcept {
    // Reference: Crypto++ sha_simd.cpp SHA256_HashMultipleBlocks_SHANI.
    // SHA-NI keeps state as (A,B,E,F) in STATE0 and (C,D,G,H) in STATE1.
    const __m128i MASK = _mm_set_epi64x(
        static_cast<long long>(0x0c0d0e0f08090a0bULL),
        static_cast<long long>(0x0405060700010203ULL));

    __m128i TMP    = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state_[0]));
    __m128i STATE1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state_[4]));

    TMP    = _mm_shuffle_epi32(TMP, 0xB1);          // CDAB
    STATE1 = _mm_shuffle_epi32(STATE1, 0x1B);       // EFGH
    __m128i STATE0 = _mm_alignr_epi8(TMP, STATE1, 8);   // ABEF
    STATE1 = _mm_blend_epi16(STATE1, TMP, 0xF0);    // CDGH

    const __m128i ABEF_SAVE = STATE0;
    const __m128i CDGH_SAVE = STATE1;

    __m128i MSG, TMSG0, TMSG1, TMSG2, TMSG3;

    // Rounds 0-3
    MSG   = _mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 0));
    TMSG0 = _mm_shuffle_epi8(MSG, MASK);
    MSG   = _mm_add_epi32(TMSG0, _mm_set_epi64x(static_cast<long long>(0xE9B5DBA5B5C0FBCFULL),
                                                static_cast<long long>(0x71374491428A2F98ULL)));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    MSG   = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

    // Rounds 4-7
    TMSG1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 16));
    TMSG1 = _mm_shuffle_epi8(TMSG1, MASK);
    MSG   = _mm_add_epi32(TMSG1, _mm_set_epi64x(static_cast<long long>(0xAB1C5ED5923F82A4ULL),
                                                static_cast<long long>(0x59F111F13956C25BULL)));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    MSG   = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    TMSG0 = _mm_sha256msg1_epu32(TMSG0, TMSG1);

    // Rounds 8-11
    TMSG2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 32));
    TMSG2 = _mm_shuffle_epi8(TMSG2, MASK);
    MSG   = _mm_add_epi32(TMSG2, _mm_set_epi64x(static_cast<long long>(0x550C7DC3243185BEULL),
                                                static_cast<long long>(0x12835B01D807AA98ULL)));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    MSG   = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    TMSG1 = _mm_sha256msg1_epu32(TMSG1, TMSG2);

    // Rounds 12-15
    TMSG3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 48));
    TMSG3 = _mm_shuffle_epi8(TMSG3, MASK);
    MSG   = _mm_add_epi32(TMSG3, _mm_set_epi64x(static_cast<long long>(0xC19BF1749BDC06A7ULL),
                                                static_cast<long long>(0x80DEB1FE72BE5D74ULL)));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP   = _mm_alignr_epi8(TMSG3, TMSG2, 4);
    TMSG0 = _mm_add_epi32(TMSG0, TMP);
    TMSG0 = _mm_sha256msg2_epu32(TMSG0, TMSG3);
    MSG   = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    TMSG2 = _mm_sha256msg1_epu32(TMSG2, TMSG3);

    // Rounds 16-19
    MSG   = _mm_add_epi32(TMSG0, _mm_set_epi64x(static_cast<long long>(0x240CA1CC0FC19DC6ULL),
                                                static_cast<long long>(0xEFBE4786E49B69C1ULL)));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP   = _mm_alignr_epi8(TMSG0, TMSG3, 4);
    TMSG1 = _mm_add_epi32(TMSG1, TMP);
    TMSG1 = _mm_sha256msg2_epu32(TMSG1, TMSG0);
    MSG   = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    TMSG3 = _mm_sha256msg1_epu32(TMSG3, TMSG0);

    // Rounds 20-23
    MSG   = _mm_add_epi32(TMSG1, _mm_set_epi64x(static_cast<long long>(0x76F988DA5CB0A9DCULL),
                                                static_cast<long long>(0x4A7484AA2DE92C6FULL)));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP   = _mm_alignr_epi8(TMSG1, TMSG0, 4);
    TMSG2 = _mm_add_epi32(TMSG2, TMP);
    TMSG2 = _mm_sha256msg2_epu32(TMSG2, TMSG1);
    MSG   = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    TMSG0 = _mm_sha256msg1_epu32(TMSG0, TMSG1);

    // Rounds 24-27
    MSG   = _mm_add_epi32(TMSG2, _mm_set_epi64x(static_cast<long long>(0xBF597FC7B00327C8ULL),
                                                static_cast<long long>(0xA831C66D983E5152ULL)));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP   = _mm_alignr_epi8(TMSG2, TMSG1, 4);
    TMSG3 = _mm_add_epi32(TMSG3, TMP);
    TMSG3 = _mm_sha256msg2_epu32(TMSG3, TMSG2);
    MSG   = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    TMSG1 = _mm_sha256msg1_epu32(TMSG1, TMSG2);

    // Rounds 28-31
    MSG   = _mm_add_epi32(TMSG3, _mm_set_epi64x(static_cast<long long>(0x1429296706CA6351ULL),
                                                static_cast<long long>(0xD5A79147C6E00BF3ULL)));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP   = _mm_alignr_epi8(TMSG3, TMSG2, 4);
    TMSG0 = _mm_add_epi32(TMSG0, TMP);
    TMSG0 = _mm_sha256msg2_epu32(TMSG0, TMSG3);
    MSG   = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    TMSG2 = _mm_sha256msg1_epu32(TMSG2, TMSG3);

    // Rounds 32-35
    MSG   = _mm_add_epi32(TMSG0, _mm_set_epi64x(static_cast<long long>(0x53380D134D2C6DFCULL),
                                                static_cast<long long>(0x2E1B213827B70A85ULL)));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP   = _mm_alignr_epi8(TMSG0, TMSG3, 4);
    TMSG1 = _mm_add_epi32(TMSG1, TMP);
    TMSG1 = _mm_sha256msg2_epu32(TMSG1, TMSG0);
    MSG   = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    TMSG3 = _mm_sha256msg1_epu32(TMSG3, TMSG0);

    // Rounds 36-39
    MSG   = _mm_add_epi32(TMSG1, _mm_set_epi64x(static_cast<long long>(0x92722C8581C2C92EULL),
                                                static_cast<long long>(0x766A0ABB650A7354ULL)));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP   = _mm_alignr_epi8(TMSG1, TMSG0, 4);
    TMSG2 = _mm_add_epi32(TMSG2, TMP);
    TMSG2 = _mm_sha256msg2_epu32(TMSG2, TMSG1);
    MSG   = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    TMSG0 = _mm_sha256msg1_epu32(TMSG0, TMSG1);

    // Rounds 40-43
    MSG   = _mm_add_epi32(TMSG2, _mm_set_epi64x(static_cast<long long>(0xC76C51A3C24B8B70ULL),
                                                static_cast<long long>(0xA81A664BA2BFE8A1ULL)));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP   = _mm_alignr_epi8(TMSG2, TMSG1, 4);
    TMSG3 = _mm_add_epi32(TMSG3, TMP);
    TMSG3 = _mm_sha256msg2_epu32(TMSG3, TMSG2);
    MSG   = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    TMSG1 = _mm_sha256msg1_epu32(TMSG1, TMSG2);

    // Rounds 44-47
    MSG   = _mm_add_epi32(TMSG3, _mm_set_epi64x(static_cast<long long>(0x106AA070F40E3585ULL),
                                                static_cast<long long>(0xD6990624D192E819ULL)));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP   = _mm_alignr_epi8(TMSG3, TMSG2, 4);
    TMSG0 = _mm_add_epi32(TMSG0, TMP);
    TMSG0 = _mm_sha256msg2_epu32(TMSG0, TMSG3);
    MSG   = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    TMSG2 = _mm_sha256msg1_epu32(TMSG2, TMSG3);

    // Rounds 48-51
    MSG   = _mm_add_epi32(TMSG0, _mm_set_epi64x(static_cast<long long>(0x34B0BCB52748774CULL),
                                                static_cast<long long>(0x1E376C0819A4C116ULL)));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP   = _mm_alignr_epi8(TMSG0, TMSG3, 4);
    TMSG1 = _mm_add_epi32(TMSG1, TMP);
    TMSG1 = _mm_sha256msg2_epu32(TMSG1, TMSG0);
    MSG   = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    TMSG3 = _mm_sha256msg1_epu32(TMSG3, TMSG0);

    // Rounds 52-55
    MSG   = _mm_add_epi32(TMSG1, _mm_set_epi64x(static_cast<long long>(0x682E6FF35B9CCA4FULL),
                                                static_cast<long long>(0x4ED8AA4A391C0CB3ULL)));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP   = _mm_alignr_epi8(TMSG1, TMSG0, 4);
    TMSG2 = _mm_add_epi32(TMSG2, TMP);
    TMSG2 = _mm_sha256msg2_epu32(TMSG2, TMSG1);
    MSG   = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

    // Rounds 56-59
    MSG   = _mm_add_epi32(TMSG2, _mm_set_epi64x(static_cast<long long>(0x8CC7020884C87814ULL),
                                                static_cast<long long>(0x78A5636F748F82EEULL)));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP   = _mm_alignr_epi8(TMSG2, TMSG1, 4);
    TMSG3 = _mm_add_epi32(TMSG3, TMP);
    TMSG3 = _mm_sha256msg2_epu32(TMSG3, TMSG2);
    MSG   = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

    // Rounds 60-63
    MSG   = _mm_add_epi32(TMSG3, _mm_set_epi64x(static_cast<long long>(0xC67178F2BEF9A3F7ULL),
                                                static_cast<long long>(0xA4506CEB90BEFFFAULL)));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    MSG   = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

    STATE0 = _mm_add_epi32(STATE0, ABEF_SAVE);
    STATE1 = _mm_add_epi32(STATE1, CDGH_SAVE);

    // Restore lane order: STATE0 = ABEF, STATE1 = CDGH → store as ABCDEFGH
    TMP    = _mm_shuffle_epi32(STATE0, 0x1B);       // FEBA
    STATE1 = _mm_shuffle_epi32(STATE1, 0xB1);       // DCHG
    STATE0 = _mm_blend_epi16(TMP, STATE1, 0xF0);    // DCBA
    STATE1 = _mm_alignr_epi8(STATE1, TMP, 8);       // ABEF

    _mm_storeu_si128(reinterpret_cast<__m128i*>(&state_[0]), STATE0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&state_[4]), STATE1);
}

#pragma GCC pop_options
#endif // ELIO_SHA256_HAVE_X86_DISPATCH

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
