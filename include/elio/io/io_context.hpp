#pragma once

#include "io_backend.hpp"
#include "io_uring_backend.hpp"
#include "epoll_backend.hpp"
#include "io_context_identity.hpp"
#include <elio/log/macros.hpp>
#include <memory>
#include <chrono>
#include <atomic>
#include <thread>
#include <stdexcept>
#include <cstdint>

namespace elio::runtime {
class worker_thread;
}

namespace elio::io {

class io_awaitable_base;
class batch_read_awaitable;
class batch_write_awaitable;

/// Unified I/O context interface.
///
/// A scheduler-owned context is bound to exactly one worker. Its backend must
/// be prepared, submitted, cancelled, and polled on that worker unless a
/// backend method explicitly documents cross-thread safety. A standalone
/// context has no scheduler owner; its caller is responsible for serializing
/// backend access and driving completion.
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
    explicit io_context(backend_type type)
        : io_context(type, detail::NO_IO_CONTEXT_OWNER) {}

    [[nodiscard]] bool is_worker_owned() const noexcept {
        return identity_->is_worker_owned();
    }

    [[nodiscard]] size_t owner_worker_id() const noexcept {
        return identity_->owner_worker_id;
    }

    [[nodiscard]] uint64_t generation() const noexcept {
        return identity_->generation;
    }

    [[nodiscard]] size_t active_pin_count() const noexcept {
        return identity_->active_pins.load(std::memory_order_acquire);
    }

private:
    friend class elio::runtime::worker_thread;
    friend class io_awaitable_base;
    friend class batch_read_awaitable;
    friend class batch_write_awaitable;
    friend void close_fd_for_destructor(int fd) noexcept;

    static std::unique_ptr<io_context> make_worker_owned(
        size_t worker_id,
        backend_type type = backend_type::auto_detect) {
        return std::unique_ptr<io_context>(new io_context(type, worker_id));
    }

    [[nodiscard]] std::shared_ptr<detail::io_context_identity>
    operation_identity() const noexcept {
        return identity_;
    }

    [[nodiscard]] io_backend* operation_backend() {
        validate_mutable_access();
        return backend_.get();
    }

    void bind_owner_thread() {
        if (!identity_->is_worker_owned()) {
            throw std::logic_error(
                "cannot bind a standalone io_context to a worker thread");
        }
        if (detail::current_worker_io_context &&
            detail::current_worker_io_context != identity_.get()) {
            throw std::logic_error(
                "one worker thread cannot own multiple io_context instances");
        }
        detail::current_worker_io_context = identity_.get();
        identity_->owner_thread_token.store(
            detail::current_io_thread_token(), std::memory_order_release);
    }

    void unbind_owner_thread() noexcept {
        if (detail::current_worker_io_context == identity_.get()) {
            detail::current_worker_io_context = nullptr;
        }
        identity_->owner_thread_token.store(nullptr, std::memory_order_release);
    }

    void validate_mutable_access() const {
        if (identity_->is_worker_owned()) {
            const bool owns_context =
                detail::current_worker_io_context == identity_.get() &&
                identity_->owner_thread_token.load(std::memory_order_acquire) ==
                    detail::current_io_thread_token();
            if (!owns_context) {
                throw std::logic_error(
                    "worker-owned io_context accessed outside its owner thread");
            }
            return;
        }

        if (detail::current_worker_io_context) {
            throw std::logic_error(
                "scheduler worker cannot mutate a standalone io_context");
        }
    }

    io_context(backend_type type, size_t owner_worker_id)
        : identity_(std::make_shared<detail::io_context_identity>(
              owner_worker_id,
              next_generation_.fetch_add(1, std::memory_order_relaxed))) {
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

public:
    ~io_context() = default;

    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;
    io_context(io_context&&) = delete;
    io_context& operator=(io_context&&) = delete;

    bool prepare(const io_request& req) {
        validate_mutable_access();
        return backend_->prepare(req);
    }

    int submit() {
        validate_mutable_access();
        return backend_->submit();
    }

    int poll(std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) {
        validate_mutable_access();
        return backend_->poll(timeout);
    }

    bool has_pending() const noexcept { return backend_->has_pending(); }
    size_t pending_count() const noexcept { return backend_->pending_count(); }

    bool cancel(void* user_data) {
        validate_mutable_access();
        return backend_->cancel(user_data);
    }

    /// Thread-safe wakeup entry; unlike backend mutation, this may be called
    /// from a foreign thread.
    void notify() noexcept { backend_->notify(); }

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

    bool is_io_uring() const noexcept {
        return backend_->is_io_uring();
    }

private:
    std::unique_ptr<io_backend> backend_;
    backend_type backend_type_ = backend_type::auto_detect;
    std::shared_ptr<detail::io_context_identity> identity_;
    inline static std::atomic<uint64_t> next_generation_{1};
};

/// Global default io_context for convenience
inline io_context& default_io_context() {
    static io_context instance;
    return instance;
}

} // namespace elio::io
