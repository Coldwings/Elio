#pragma once

/// @file cpu_features.hpp
/// @brief CPU feature detection for hardware-accelerated hash functions

#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define ELIO_HASH_X86 1
#include <cpuid.h>
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define ELIO_HASH_ARM64 1
#if defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#endif

namespace elio::hash {

/// CPU features relevant for hash acceleration
struct cpu_features {
    // x86 features
    bool sse42 = false;      ///< SSE4.2 (includes CRC32 instruction)
    bool avx = false;        ///< AVX
    bool avx2 = false;       ///< AVX2
    bool sha_ni = false;     ///< SHA-NI (Intel SHA extensions)
    
    // ARM features
    bool arm_crc32 = false;  ///< ARM CRC32 instructions
    bool arm_sha1 = false;   ///< ARM SHA1 instructions
    bool arm_sha2 = false;   ///< ARM SHA256 instructions
    
    /// Get CPU features (cached, thread-safe)
    static const cpu_features& get() noexcept {
        static const cpu_features instance = detect();
        return instance;
    }
    
private:
    static cpu_features detect() noexcept {
        cpu_features f;
        
#ifdef ELIO_HASH_X86
        unsigned int eax, ebx, ecx, edx;
        
        // Check if CPUID is supported and get max function
        if (__get_cpuid_max(0, nullptr) >= 1) {
            __cpuid(1, eax, ebx, ecx, edx);
            f.sse42 = (ecx & (1 << 20)) != 0;  // SSE4.2
            f.avx = (ecx & (1 << 28)) != 0;    // AVX
        }
        
        if (__get_cpuid_max(0, nullptr) >= 7) {
            __cpuid_count(7, 0, eax, ebx, ecx, edx);
            f.avx2 = (ebx & (1 << 5)) != 0;    // AVX2
            f.sha_ni = (ebx & (1 << 29)) != 0; // SHA-NI
        }
#endif

#ifdef ELIO_HASH_ARM64
#if defined(__linux__)
        unsigned long hwcap = getauxval(AT_HWCAP);
        f.arm_crc32 = (hwcap & HWCAP_CRC32) != 0;
        f.arm_sha1 = (hwcap & HWCAP_SHA1) != 0;
        f.arm_sha2 = (hwcap & HWCAP_SHA2) != 0;
#elif defined(__APPLE__)
        // On Apple Silicon, CRC32 and SHA are always available
        f.arm_crc32 = true;
        f.arm_sha1 = true;
        f.arm_sha2 = true;
#elif defined(__ARM_FEATURE_CRC32)
        // Compile-time detection fallback
        f.arm_crc32 = true;
#endif
#endif
        
        return f;
    }
};

/// Check if hardware CRC32 is available
inline bool has_hw_crc32() noexcept {
#ifdef ELIO_HASH_X86
    return cpu_features::get().sse42;
#elif defined(ELIO_HASH_ARM64)
    return cpu_features::get().arm_crc32;
#else
    return false;
#endif
}

/// Check if SHA-NI is available
inline bool has_sha_ni() noexcept {
#ifdef ELIO_HASH_X86
    return cpu_features::get().sha_ni;
#else
    return false;
#endif
}

/// Check if ARM SHA instructions are available
inline bool has_arm_sha() noexcept {
#ifdef ELIO_HASH_ARM64
    return cpu_features::get().arm_sha1 && cpu_features::get().arm_sha2;
#else
    return false;
#endif
}

} // namespace elio::hash
