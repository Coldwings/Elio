#pragma once

/// @file crc32.hpp
/// @brief Optimized CRC32 checksum implementation
///
/// Provides CRC32 checksum computation using the IEEE polynomial (0xEDB88320).
/// Features:
/// - Slicing-by-8 algorithm for high-performance software CRC32
/// - Hardware CRC32C acceleration using SSE4.2 intrinsics (x86/x64)
/// - Hardware CRC32/CRC32C acceleration using ACLE intrinsics (AArch64)
/// - Aligned memory access optimization
/// - Loop unrolling for improved throughput

#include "cpu_features.hpp"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <span>
#include <sys/uio.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define ELIO_CRC32_X86 1
#if defined(__SSE4_2__)
#include <nmmintrin.h>
#define ELIO_CRC32_HW_X86 1
#endif
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define ELIO_CRC32_ARM64 1
#if defined(__ARM_FEATURE_CRC32) || defined(__APPLE__)
#include <arm_acle.h>
#define ELIO_CRC32_HW_ARM64 1
#endif
#endif

namespace elio::hash {

// ============================================================================
// CRC32 lookup tables (IEEE polynomial 0xEDB88320)
// Slicing-by-8 tables for high performance
// ============================================================================

namespace detail {

// Generate CRC32 tables at compile time
constexpr uint32_t crc32_make_table_entry(uint32_t n, int k = 8) {
    return k == 0 ? n : crc32_make_table_entry((n & 1) ? (0xEDB88320 ^ (n >> 1)) : (n >> 1), k - 1);
}

template<size_t... I>
constexpr auto crc32_make_table(std::index_sequence<I...>) {
    return std::array<uint32_t, sizeof...(I)>{{ crc32_make_table_entry(I)... }};
}

// Main CRC32 table
constexpr auto crc32_table = crc32_make_table(std::make_index_sequence<256>{});

// Slicing-by-8 tables
struct crc32_tables {
    uint32_t t[8][256];
    
    constexpr crc32_tables() : t{} {
        // Table 0 is the standard byte-at-a-time table
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int j = 0; j < 8; ++j) {
                crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
            }
            t[0][i] = crc;
        }
        
        // Tables 1-7 are derived from table 0
        for (uint32_t i = 0; i < 256; ++i) {
            t[1][i] = (t[0][i] >> 8) ^ t[0][t[0][i] & 0xFF];
            t[2][i] = (t[1][i] >> 8) ^ t[0][t[1][i] & 0xFF];
            t[3][i] = (t[2][i] >> 8) ^ t[0][t[2][i] & 0xFF];
            t[4][i] = (t[3][i] >> 8) ^ t[0][t[3][i] & 0xFF];
            t[5][i] = (t[4][i] >> 8) ^ t[0][t[4][i] & 0xFF];
            t[6][i] = (t[5][i] >> 8) ^ t[0][t[5][i] & 0xFF];
            t[7][i] = (t[6][i] >> 8) ^ t[0][t[6][i] & 0xFF];
        }
    }
};

inline constexpr crc32_tables crc32_slice_tables{};

// Software CRC32 using slicing-by-8
inline uint32_t crc32_sw(const uint8_t* data, size_t length, uint32_t crc) noexcept {
    const auto& t = crc32_slice_tables.t;
    
    // Process unaligned head bytes
    while (length > 0 && (reinterpret_cast<uintptr_t>(data) & 7)) {
        crc = t[0][(crc ^ *data++) & 0xFF] ^ (crc >> 8);
        --length;
    }
    
    // Process 8 bytes at a time (slicing-by-8)
    while (length >= 8) {
        uint32_t lo, hi;
        std::memcpy(&lo, data, 4);
        std::memcpy(&hi, data + 4, 4);
        crc ^= lo;
        
        crc = t[7][crc & 0xFF] ^
              t[6][(crc >> 8) & 0xFF] ^
              t[5][(crc >> 16) & 0xFF] ^
              t[4][(crc >> 24)] ^
              t[3][hi & 0xFF] ^
              t[2][(hi >> 8) & 0xFF] ^
              t[1][(hi >> 16) & 0xFF] ^
              t[0][(hi >> 24)];
        
        data += 8;
        length -= 8;
    }
    
    // Process remaining bytes
    while (length--) {
        crc = t[0][(crc ^ *data++) & 0xFF] ^ (crc >> 8);
    }
    
    return crc;
}

#ifdef ELIO_CRC32_HW_X86
// Hardware-accelerated CRC32C using SSE4.2
// Note: This uses the Castagnoli polynomial (0x1EDC6F41), not IEEE
inline uint32_t crc32c_hw_x86(const uint8_t* data, size_t length, uint32_t crc) noexcept {
    // Process unaligned head bytes
    while (length > 0 && (reinterpret_cast<uintptr_t>(data) & 7)) {
        crc = _mm_crc32_u8(crc, *data++);
        --length;
    }
    
#ifdef __x86_64__
    // Process 8 bytes at a time on 64-bit
    while (length >= 8) {
        uint64_t val;
        std::memcpy(&val, data, 8);
        crc = static_cast<uint32_t>(_mm_crc32_u64(crc, val));
        data += 8;
        length -= 8;
    }
#endif
    
    // Process 4 bytes at a time
    while (length >= 4) {
        uint32_t val;
        std::memcpy(&val, data, 4);
        crc = _mm_crc32_u32(crc, val);
        data += 4;
        length -= 4;
    }
    
    // Process remaining bytes
    while (length--) {
        crc = _mm_crc32_u8(crc, *data++);
    }
    
    return crc;
}
#endif // ELIO_CRC32_HW_X86

#ifdef ELIO_CRC32_HW_ARM64
// Hardware-accelerated CRC32 using ARM ACLE (IEEE polynomial)
inline uint32_t crc32_hw_arm64(const uint8_t* data, size_t length, uint32_t crc) noexcept {
    // Process unaligned head bytes
    while (length > 0 && (reinterpret_cast<uintptr_t>(data) & 7)) {
        crc = __crc32b(crc, *data++);
        --length;
    }
    
    // Process 8 bytes at a time
    while (length >= 8) {
        uint64_t val;
        std::memcpy(&val, data, 8);
        crc = __crc32d(crc, val);
        data += 8;
        length -= 8;
    }
    
    // Process 4 bytes at a time
    while (length >= 4) {
        uint32_t val;
        std::memcpy(&val, data, 4);
        crc = __crc32w(crc, val);
        data += 4;
        length -= 4;
    }
    
    // Process remaining bytes
    while (length--) {
        crc = __crc32b(crc, *data++);
    }
    
    return crc;
}

// Hardware-accelerated CRC32C using ARM ACLE (Castagnoli polynomial)
inline uint32_t crc32c_hw_arm64(const uint8_t* data, size_t length, uint32_t crc) noexcept {
    // Process unaligned head bytes
    while (length > 0 && (reinterpret_cast<uintptr_t>(data) & 7)) {
        crc = __crc32cb(crc, *data++);
        --length;
    }
    
    // Process 8 bytes at a time
    while (length >= 8) {
        uint64_t val;
        std::memcpy(&val, data, 8);
        crc = __crc32cd(crc, val);
        data += 8;
        length -= 8;
    }
    
    // Process 4 bytes at a time
    while (length >= 4) {
        uint32_t val;
        std::memcpy(&val, data, 4);
        crc = __crc32cw(crc, val);
        data += 4;
        length -= 4;
    }
    
    // Process remaining bytes
    while (length--) {
        crc = __crc32cb(crc, *data++);
    }
    
    return crc;
}
#endif // ELIO_CRC32_HW_ARM64

} // namespace detail

// ============================================================================
// CRC32 functions (IEEE polynomial)
// ============================================================================

/// Compute CRC32 checksum of a buffer (IEEE polynomial)
/// Uses hardware acceleration on ARM64, slicing-by-8 on other platforms
/// @param data Pointer to data buffer
/// @param length Length of data in bytes
/// @param crc Initial CRC value (default 0xFFFFFFFF for new computation)
/// @return Final CRC32 checksum
inline uint32_t crc32(const void* data, size_t length, uint32_t crc = 0xFFFFFFFF) noexcept {
    if (length == 0) return crc ^ 0xFFFFFFFF;
    
#ifdef ELIO_CRC32_HW_ARM64
    if (has_hw_crc32()) {
        return detail::crc32_hw_arm64(static_cast<const uint8_t*>(data), length, crc) ^ 0xFFFFFFFF;
    }
#endif
    
    return detail::crc32_sw(static_cast<const uint8_t*>(data), length, crc) ^ 0xFFFFFFFF;
}

/// Compute CRC32 checksum of a span
inline uint32_t crc32(std::span<const uint8_t> data, uint32_t crc = 0xFFFFFFFF) noexcept {
    return crc32(data.data(), data.size(), crc);
}

/// Compute CRC32 checksum over multiple iovec buffers (scatter-gather)
inline uint32_t crc32_iovec(const struct iovec* iov, size_t count) noexcept {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < count; ++i) {
        if (iov[i].iov_len > 0) {
#ifdef ELIO_CRC32_HW_ARM64
            if (has_hw_crc32()) {
                crc = detail::crc32_hw_arm64(static_cast<const uint8_t*>(iov[i].iov_base), 
                                             iov[i].iov_len, crc);
                continue;
            }
#endif
            crc = detail::crc32_sw(static_cast<const uint8_t*>(iov[i].iov_base), 
                                   iov[i].iov_len, crc);
        }
    }
    return crc ^ 0xFFFFFFFF;
}

/// Update CRC32 computation incrementally (does not finalize)
inline uint32_t crc32_update(const void* data, size_t length, uint32_t crc) noexcept {
    if (length == 0) return crc;
    
#ifdef ELIO_CRC32_HW_ARM64
    if (has_hw_crc32()) {
        return detail::crc32_hw_arm64(static_cast<const uint8_t*>(data), length, crc);
    }
#endif
    
    return detail::crc32_sw(static_cast<const uint8_t*>(data), length, crc);
}

/// Finalize incremental CRC32 computation
inline uint32_t crc32_finalize(uint32_t crc) noexcept {
    return crc ^ 0xFFFFFFFF;
}

// ============================================================================
// CRC32C functions (Castagnoli polynomial, hardware accelerated)
// ============================================================================

/// Compute CRC32C checksum (Castagnoli polynomial)
/// Uses hardware acceleration on x86 with SSE4.2 or ARM64, falls back to software
/// @param data Pointer to data buffer
/// @param length Length of data in bytes
/// @param crc Initial CRC value (default 0xFFFFFFFF)
/// @return Final CRC32C checksum
inline uint32_t crc32c(const void* data, size_t length, uint32_t crc = 0xFFFFFFFF) noexcept {
    if (length == 0) return crc ^ 0xFFFFFFFF;
    
#ifdef ELIO_CRC32_HW_X86
    if (has_hw_crc32()) {
        return detail::crc32c_hw_x86(static_cast<const uint8_t*>(data), length, crc) ^ 0xFFFFFFFF;
    }
#endif

#ifdef ELIO_CRC32_HW_ARM64
    if (has_hw_crc32()) {
        return detail::crc32c_hw_arm64(static_cast<const uint8_t*>(data), length, crc) ^ 0xFFFFFFFF;
    }
#endif
    
    // Software fallback for CRC32C (Castagnoli polynomial)
    // Using slicing-by-4 with Castagnoli tables
    static const auto make_crc32c_table = []() {
        std::array<uint32_t, 256> table{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                c = (c >> 1) ^ ((c & 1) ? 0x82F63B78 : 0);
            }
            table[i] = c;
        }
        return table;
    };
    static const auto crc32c_table = make_crc32c_table();
    
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; ++i) {
        crc = crc32c_table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

/// Compute CRC32C checksum of a span
inline uint32_t crc32c(std::span<const uint8_t> data, uint32_t crc = 0xFFFFFFFF) noexcept {
    return crc32c(data.data(), data.size(), crc);
}

/// Check if hardware CRC32C is available
inline bool crc32c_hw_available() noexcept {
    return has_hw_crc32();
}

} // namespace elio::hash
