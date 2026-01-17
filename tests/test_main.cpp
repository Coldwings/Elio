// Test main - Catch2 provides main via Catch2::Catch2WithMain
// This file is intentionally empty but included for future test setup hooks

// Helper to detect sanitizers and scale timeouts accordingly
#ifndef ELIO_TEST_HELPERS_HPP
#define ELIO_TEST_HELPERS_HPP

#include <chrono>

namespace elio::test {

// Detect if running under ThreadSanitizer
constexpr bool is_tsan_enabled() {
#if defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
    #if __has_feature(thread_sanitizer)
        return true;
    #else
        return false;
    #endif
#else
    return false;
#endif
}

// Detect if running under AddressSanitizer
constexpr bool is_asan_enabled() {
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
    #if __has_feature(address_sanitizer)
        return true;
    #else
        return false;
    #endif
#else
    return false;
#endif
}

// Scale factor for timeouts under sanitizers
constexpr int timeout_scale_factor() {
    if (is_tsan_enabled()) return 10;  // TSAN is ~5-15x slower
    if (is_asan_enabled()) return 3;   // ASAN is ~2-3x slower
    return 1;
}

// Helper to scale milliseconds timeout
inline std::chrono::milliseconds scaled_ms(int base_ms) {
    return std::chrono::milliseconds(base_ms * timeout_scale_factor());
}

// Helper to scale seconds timeout
inline std::chrono::seconds scaled_sec(int base_sec) {
    return std::chrono::seconds(base_sec * timeout_scale_factor());
}

} // namespace elio::test

#endif // ELIO_TEST_HELPERS_HPP