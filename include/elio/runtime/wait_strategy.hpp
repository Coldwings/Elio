#pragma once

#include <cstddef>
#include <thread>

namespace elio::runtime {

/// Configuration for how workers wait when idle
/// Controls the trade-off between latency and CPU usage
struct wait_strategy {
    /// Number of spin iterations before blocking (0 = pure blocking)
    size_t spin_iterations = 0;

    /// Whether to yield during spin (true = more friendly to other threads)
    bool spin_yield = false;

    // === Preset strategies ===

    /// Pure blocking - lowest CPU usage, slightly higher wake latency
    /// Default strategy, suitable for most workloads
    static constexpr wait_strategy blocking() noexcept {
        return {0, false};
    }

    /// Pure spinning - lowest latency but burns CPU
    /// Use only when latency is critical and CPU is dedicated
    static constexpr wait_strategy spinning(size_t iterations) noexcept {
        return {iterations, false};
    }

    /// Hybrid spin-then-block - good balance for low-latency workloads
    /// Yields during spin to be friendly to other threads
    static constexpr wait_strategy hybrid(size_t iterations) noexcept {
        return {iterations, true};
    }

    /// Aggressive hybrid - more spinning for ultra-low latency
    /// Uses pause instruction instead of yield
    static constexpr wait_strategy aggressive(size_t iterations = 1000) noexcept {
        return {iterations, false};
    }
};

/// CPU relax instruction for spin loops
/// Reduces power consumption and improves SMT performance
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
    asm volatile("yield" ::: "memory");
#elif defined(__arm__) || defined(_M_ARM)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

} // namespace elio::runtime
