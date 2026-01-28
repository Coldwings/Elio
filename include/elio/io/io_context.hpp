#pragma once

#include "io_backend.hpp"
#include "io_uring_backend.hpp"
#include "epoll_backend.hpp"
#include <elio/log/macros.hpp>
#include <memory>
#include <chrono>
#include <atomic>
#include <thread>

namespace elio::io {

/// Unified I/O context interface
/// Abstracts over io_uring and epoll backends with automatic detection
class io_context {
public:
    /// Backend type selection
    enum class backend_type {
        auto_detect,  ///< Automatically choose best available
        io_uring,     ///< Use io_uring (Linux 5.1+)
        epoll         ///< Use epoll (fallback)
    };
    
    /// Constructor with auto-detection
    /// Detects io_uring availability and falls back to epoll
    io_context() : io_context(backend_type::auto_detect) {}
    
    /// Constructor with explicit backend selection
    explicit io_context(backend_type type) {
        switch (type) {
            case backend_type::auto_detect:
#if ELIO_HAS_IO_URING
                if (io_uring_backend::is_available()) {
                    try {
                        backend_ = std::make_unique<io_uring_backend>();
                        backend_type_ = backend_type::io_uring;
                        ELIO_LOG_INFO("io_context using io_uring backend (auto-detected)");
                        return;
                    } catch (const std::exception& e) {
                        ELIO_LOG_WARNING("io_uring init failed: {}, falling back to epoll", e.what());
                    }
                }
#endif
                backend_ = std::make_unique<epoll_backend>();
                backend_type_ = backend_type::epoll;
                ELIO_LOG_INFO("io_context using epoll backend");
                break;
                
            case backend_type::io_uring:
#if ELIO_HAS_IO_URING
                backend_ = std::make_unique<io_uring_backend>();
                backend_type_ = backend_type::io_uring;
                ELIO_LOG_INFO("io_context using io_uring backend (explicit)");
#else
                throw std::runtime_error("io_uring backend not available (not compiled with liburing)");
#endif
                break;
                
            case backend_type::epoll:
                backend_ = std::make_unique<epoll_backend>();
                backend_type_ = backend_type::epoll;
                ELIO_LOG_INFO("io_context using epoll backend (explicit)");
                break;
        }
    }
    
    /// Destructor
    ~io_context() = default;
    
    // Non-copyable, movable
    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;
    io_context(io_context&&) = default;
    io_context& operator=(io_context&&) = default;
    
    /// Prepare an I/O operation
    /// @param req The I/O request to prepare
    /// @return true if prepared successfully
    bool prepare(const io_request& req) {
        return backend_->prepare(req);
    }
    
    /// Submit all prepared operations
    /// @return Number of operations submitted
    int submit() {
        return backend_->submit();
    }
    
    /// Poll for completed I/O operations
    /// @param timeout Maximum time to wait
    /// @return Number of completions processed
    int poll(std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) {
        return backend_->poll(timeout);
    }
    
    /// Check if there are pending I/O operations
    bool has_pending() const noexcept {
        return backend_->has_pending();
    }
    
    /// Get number of pending operations
    size_t pending_count() const noexcept {
        return backend_->pending_count();
    }
    
    /// Cancel a pending operation
    bool cancel(void* user_data) {
        return backend_->cancel(user_data);
    }
    
    /// Get the active backend type
    backend_type get_backend_type() const noexcept {
        return backend_type_;
    }
    
    /// Get backend type as string
    const char* get_backend_name() const noexcept {
        switch (backend_type_) {
            case backend_type::io_uring: return "io_uring";
            case backend_type::epoll: return "epoll";
            default: return "unknown";
        }
    }
    
    /// Get the last I/O result (thread-local)
    static io_result get_last_result() noexcept {
#if ELIO_HAS_IO_URING
        // Check which backend produced the result
        // For now, check io_uring first since it's preferred
        auto result = io_uring_backend::get_last_result();
        if (result.result != 0 || result.flags != 0) {
            return result;
        }
#endif
        return epoll_backend::get_last_result();
    }
    
    /// Run the event loop until stopped
    /// @param stop_flag Atomic flag to signal stop
    void run(std::atomic<bool>& stop_flag) {
        while (!stop_flag.load(std::memory_order_relaxed)) {
            if (has_pending()) {
                poll(std::chrono::milliseconds(10));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    
    /// Run the event loop for a specific duration
    void run_for(std::chrono::milliseconds duration) {
        auto end_time = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < end_time) {
            if (has_pending()) {
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    end_time - std::chrono::steady_clock::now());
                if (remaining.count() > 0) {
                    poll(std::min(remaining, std::chrono::milliseconds(10)));
                }
            } else {
                break;  // No pending operations
            }
        }
    }
    
    /// Run until all pending operations complete
    void run_until_complete() {
        while (has_pending()) {
            poll(std::chrono::milliseconds(10));
        }
    }
    
    /// Get the underlying backend (for advanced use)
    io_backend* get_backend() noexcept {
        return backend_.get();
    }

    /// Check if the current backend is io_uring
    /// @return true if using io_uring backend
    bool is_io_uring() const noexcept {
        return backend_ && backend_->is_io_uring();
    }

private:
    std::unique_ptr<io_backend> backend_;   ///< Active I/O backend
    backend_type backend_type_;              ///< Current backend type
};

/// Global default io_context for convenience
/// Created lazily on first use using Meyer's singleton pattern.
/// The destructor is safe to call during static destruction.
inline io_context& default_io_context() {
    static io_context instance;
    return instance;
}

} // namespace elio::io
