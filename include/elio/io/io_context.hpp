#pragma once

#include "io_backend.hpp"
#include "io_uring_backend.hpp"
#include "epoll_backend.hpp"
#include <elio/log/macros.hpp>
#include <memory>
#include <chrono>
#include <atomic>
#include <thread>
#include <stdexcept>

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
    io_context() : io_context(backend_type::auto_detect) {}

    /// Constructor with explicit backend selection
    explicit io_context(backend_type type) {
        switch (type) {
            case backend_type::auto_detect:
#if ELIO_HAS_IO_URING
                try {
                    backend_ = std::make_unique<io_uring_backend>();
                    backend_type_ = backend_type::io_uring;
                    ELIO_LOG_INFO("io_context using io_uring backend (auto-detected)");
                    return;
                } catch (const std::exception& ex) {
                    ELIO_LOG_WARNING(
                        "io_context auto-detect could not initialize io_uring: {}; "
                        "falling back to epoll",
                        ex.what());
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

    ~io_context() = default;

    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;
    io_context(io_context&&) = delete;
    io_context& operator=(io_context&&) = delete;

    io_backend& backend() noexcept {
        return *backend_;
    }

    const io_backend& backend() const noexcept {
        return *backend_;
    }

    bool prepare(const io_request& req) { return backend().prepare(req); }
    int submit() { return backend().submit(); }

    int poll(std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) {
        return backend().poll(timeout);
    }

    bool has_pending() const noexcept { return backend().has_pending(); }
    size_t pending_count() const noexcept { return backend().pending_count(); }
    bool cancel(void* user_data) { return backend().cancel(user_data); }
    void notify() { backend().notify(); }

    backend_type get_backend_type() const noexcept {
        return backend_type_;
    }

    const char* get_backend_name() const noexcept {
        switch (backend_type_) {
            case backend_type::io_uring: return "io_uring";
            case backend_type::epoll: return "epoll";
            default: return "unknown";
        }
    }

    static io_result get_last_result() noexcept {
#if ELIO_HAS_IO_URING
        return io_uring_backend::get_last_result();
#else
        return epoll_backend::get_last_result();
#endif
    }

    void run(std::atomic<bool>& stop_flag) {
        while (!stop_flag.load(std::memory_order_relaxed)) {
            poll(std::chrono::milliseconds(-1));
        }
    }

    void stop(std::atomic<bool>& stop_flag) {
        stop_flag.store(true, std::memory_order_release);
        notify();
    }

    void run_for(std::chrono::milliseconds duration) {
        auto end_time = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < end_time) {
            // Non-blocking drain: process any already-ready CQEs without
            // blocking, so an idle context returns promptly.
            poll(std::chrono::milliseconds(0));
            if (!has_pending()) break;
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                end_time - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) break;
            // Block for remaining time waiting for new CQEs to arrive.
            poll(remaining);
        }
    }

    void run_until_complete() {
        while (has_pending()) {
            poll(std::chrono::milliseconds(-1));
        }
    }

    io_backend* get_backend() noexcept { return backend_.get(); }

    bool is_io_uring() const noexcept {
        return backend_->is_io_uring();
    }

private:
    std::unique_ptr<io_backend> backend_;
    backend_type backend_type_ = backend_type::auto_detect;
};

/// Global default io_context for convenience
inline io_context& default_io_context() {
    static io_context instance;
    return instance;
}

} // namespace elio::io
