#pragma once

#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/chrono.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <chrono>

namespace elio::log {

/// Log level enumeration
enum class level {
    debug = 0,
    info = 1,
    warning = 2,
    error = 3
};

/// Convert log level to string
constexpr const char* level_to_string(level lvl) noexcept {
    switch (lvl) {
        case level::debug:   return "DEBUG";
        case level::info:    return "INFO";
        case level::warning: return "WARN";
        case level::error:   return "ERROR";
        default:             return "UNKNOWN";
    }
}

/// Convert log level to ANSI color code
constexpr const char* level_to_color(level lvl) noexcept {
    switch (lvl) {
        case level::debug:   return "\033[36m";  // Cyan
        case level::info:    return "\033[32m";  // Green
        case level::warning: return "\033[33m";  // Yellow
        case level::error:   return "\033[31m";  // Red
        default:             return "\033[0m";   // Reset
    }
}

/// Singleton logger class
class logger {
public:
    /// Get singleton instance
    static logger& instance() noexcept {
        static logger inst;
        return inst;
    }

    /// Set minimum log level
    void set_level(level min_level) noexcept {
        min_level_.store(min_level, std::memory_order_relaxed);
    }

    /// Get current minimum log level
    level get_level() const noexcept {
        return min_level_.load(std::memory_order_relaxed);
    }

    /// Log a message with formatting
    template<typename... Args>
    void log(level lvl, const char* file, int line, fmt::format_string<Args...> fmt_str, Args&&... args) {
        // Check if this log level should be output
        if (lvl < min_level_.load(std::memory_order_relaxed)) {
            return;
        }

        // Get current time
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        // Format into a thread-local buffer OUTSIDE the global lock so that
        // formatting (which can be expensive) runs concurrently across threads.
        // Each thread reuses its own buffer to avoid malloc churn.
        thread_local fmt::memory_buffer buf;
        buf.clear();
        try {
            // Format: [TIMESTAMP] [LEVEL] [file:line] message<RESET>\n
            // The user message is formatted directly into the buffer instead of
            // being materialized as an intermediate std::string.
            fmt::format_to(std::back_inserter(buf),
                "{}[{:%Y-%m-%d %H:%M:%S}.{:03d}] [{}] [{}:{}] ",
                level_to_color(lvl),
                fmt::localtime(time),
                ms.count(),
                level_to_string(lvl),
                file,
                line);
            fmt::format_to(std::back_inserter(buf), fmt_str, std::forward<Args>(args)...);
            fmt::format_to(std::back_inserter(buf), "\033[0m\n");
        } catch (...) {
            // If formatting throws (e.g. invalid argument at runtime), make sure
            // we don't leave a partially-formatted line lying around for the next
            // call, and don't pin a now-grown allocation either.
            buf = fmt::memory_buffer{};
            throw;
        }

        // Lock only around the write syscall so the global mutex is held for
        // the shortest possible window. lock_guard guarantees release even if
        // fwrite somehow throws.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::fwrite(buf.data(), 1, buf.size(), stderr);
        }

        // Cap the thread-local buffer so a single huge log line doesn't pin a
        // large allocation forever. fmt::memory_buffer has no shrink_to_fit, so
        // assign a fresh empty buffer when we cross the threshold.
        if (buf.capacity() > kBufferShrinkThreshold) {
            buf = fmt::memory_buffer{};
        }
    }

private:
    logger() noexcept : min_level_(level::info) {}
    ~logger() = default;
    
    logger(const logger&) = delete;
    logger& operator=(const logger&) = delete;

    /// Threshold above which the thread-local format buffer is reset to avoid
    /// keeping a large allocation alive after an unusually long log line.
    static constexpr std::size_t kBufferShrinkThreshold = 64 * 1024;

    std::atomic<level> min_level_;
    std::mutex mutex_;  // Protect concurrent writes to stderr
};

} // namespace elio::log
