#pragma once

#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/chrono.h>
#include <atomic>
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

        // Format the message
        auto msg = fmt::format(fmt_str, std::forward<Args>(args)...);
        
        // Get current time
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        // Thread-safe output
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Format: [TIMESTAMP] [LEVEL] [file:line] message
        fmt::print(stderr, 
            "{}[{:%Y-%m-%d %H:%M:%S}.{:03d}] [{}] [{}:{}] {}\033[0m\n",
            level_to_color(lvl),
            fmt::localtime(time),
            ms.count(),
            level_to_string(lvl),
            file,
            line,
            msg
        );
    }

private:
    logger() noexcept : min_level_(level::info) {}
    ~logger() = default;
    
    logger(const logger&) = delete;
    logger& operator=(const logger&) = delete;

    std::atomic<level> min_level_;
    std::mutex mutex_;  // Protect concurrent writes to stderr
};

} // namespace elio::log
