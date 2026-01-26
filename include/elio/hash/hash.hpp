#pragma once

/// @file hash.hpp
/// @brief Hash and checksum functions
///
/// This module provides various hash and checksum implementations:
/// - CRC32: Fast checksum for data integrity
/// - SHA-1: Cryptographic hash (legacy, use SHA-256 for security)
/// - SHA-256: Strong cryptographic hash
///
/// All implementations are header-only with no external dependencies.

#include "crc32.hpp"
#include "sha1.hpp"
#include "sha256.hpp"

namespace elio::hash {

// ============================================================================
// Common utilities
// ============================================================================

/// Convert any digest array to hexadecimal string
template<size_t N>
inline std::string to_hex(const std::array<uint8_t, N>& digest) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(N * 2);
    for (uint8_t byte : digest) {
        result += hex_chars[(byte >> 4) & 0x0F];
        result += hex_chars[byte & 0x0F];
    }
    return result;
}

/// Convert raw bytes to hexadecimal string
inline std::string to_hex(const void* data, size_t length) {
    static const char hex_chars[] = "0123456789abcdef";
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    std::string result;
    result.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        result += hex_chars[(bytes[i] >> 4) & 0x0F];
        result += hex_chars[bytes[i] & 0x0F];
    }
    return result;
}

} // namespace elio::hash
