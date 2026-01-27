#pragma once

#include <algorithm>
#include <atomic>
#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace elio::coro {

/// Result of a cancellable operation
enum class cancel_result {
    completed,   ///< Operation completed normally
    cancelled    ///< Operation was cancelled
};

namespace detail {

/// Shared cancellation state (implementation detail)
struct cancel_state {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::vector<std::pair<uint64_t, std::function<void()>>> callbacks;
    uint64_t next_id = 1;
    
    uint64_t add_callback(std::function<void()> cb) {
        std::lock_guard<std::mutex> lock(mutex);
        if (cancelled.load(std::memory_order_relaxed)) {
            // Already cancelled, invoke immediately outside lock
            // Need to release lock first
            mutex.unlock();
            cb();
            mutex.lock();
            return 0;
        }
        uint64_t id = next_id++;
        callbacks.emplace_back(id, std::move(cb));
        return id;
    }
    
    void remove_callback(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex);
        callbacks.erase(
            std::remove_if(callbacks.begin(), callbacks.end(),
                [id](const auto& p) { return p.first == id; }),
            callbacks.end()
        );
    }
    
    void trigger() {
        std::vector<std::function<void()>> to_invoke;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (cancelled.exchange(true, std::memory_order_release)) {
                return; // Already cancelled
            }
            for (auto& [id, cb] : callbacks) {
                to_invoke.push_back(std::move(cb));
            }
            callbacks.clear();
        }
        // Invoke callbacks outside the lock
        for (auto& cb : to_invoke) {
            cb();
        }
    }
};

} // namespace detail

/// Forward declaration
class cancel_source;

/// Registration handle for cancel callbacks
class cancel_registration {
public:
    cancel_registration() = default;
    cancel_registration(cancel_registration&& other) noexcept 
        : state_(std::move(other.state_)), id_(other.id_) {
        other.id_ = 0;
    }
    cancel_registration& operator=(cancel_registration&& other) noexcept {
        if (this != &other) {
            unregister();
            state_ = std::move(other.state_);
            id_ = other.id_;
            other.id_ = 0;
        }
        return *this;
    }
    ~cancel_registration() { unregister(); }
    
    // Non-copyable
    cancel_registration(const cancel_registration&) = delete;
    cancel_registration& operator=(const cancel_registration&) = delete;
    
    /// Manually unregister the callback
    void unregister() {
        if (state_ && id_ != 0) {
            state_->remove_callback(id_);
            id_ = 0;
        }
    }
    
private:
    template<typename> friend class basic_cancel_token;
    friend class cancel_token;
    
    cancel_registration(std::shared_ptr<detail::cancel_state> state, uint64_t id)
        : state_(std::move(state)), id_(id) {}
    
    std::shared_ptr<detail::cancel_state> state_;
    uint64_t id_ = 0;
};

/// A token that can be used to check for and respond to cancellation requests.
/// 
/// cancel_token is a lightweight handle that can be copied and passed to
/// functions that should be cancellable. Multiple tokens can share the same
/// cancellation state via a cancel_source.
///
/// Example:
/// ```cpp
/// task<void> cancellable_work(cancel_token token) {
///     while (!token.is_cancelled()) {
///         // do work
///         auto result = co_await time::sleep_for(100ms, token);
///         if (result == cancel_result::cancelled) break;
///     }
/// }
/// ```
class cancel_token {
public:
    using registration = cancel_registration;
    
    /// Default constructor creates an empty (never-cancelled) token
    cancel_token() = default;
    
    /// Check if cancellation has been requested
    bool is_cancelled() const noexcept {
        return state_ && state_->cancelled.load(std::memory_order_acquire);
    }
    
    /// Implicit conversion to bool - returns true if NOT cancelled
    /// Allows: if (token) { /* not cancelled */ }
    explicit operator bool() const noexcept {
        return !is_cancelled();
    }
    
    /// Register a callback to be invoked when cancellation is requested.
    /// The callback will be invoked immediately if already cancelled.
    /// @param callback Function to call on cancellation
    /// @return Registration handle (callback unregisters when handle is destroyed)
    template<typename F>
    [[nodiscard]] registration on_cancel(F&& callback) const {
        if (!state_) {
            return registration{};
        }
        return registration{state_, state_->add_callback(std::forward<F>(callback))};
    }
    
    /// Register a coroutine handle to be resumed when cancelled.
    /// @param handle Coroutine to resume on cancellation
    /// @return Registration handle
    [[nodiscard]] registration on_cancel_resume(std::coroutine_handle<> handle) const {
        return on_cancel([handle]() {
            if (handle && !handle.done()) {
                handle.resume();
            }
        });
    }

private:
    friend class cancel_source;
    
    explicit cancel_token(std::shared_ptr<detail::cancel_state> state)
        : state_(std::move(state)) {}
    
    std::shared_ptr<detail::cancel_state> state_;
};

/// A source of cancellation that can create tokens and trigger cancellation.
///
/// cancel_source owns the cancellation state and can create multiple tokens
/// that share the same state. When cancel() is called, all associated tokens
/// become cancelled and their registered callbacks are invoked.
///
/// Example:
/// ```cpp
/// cancel_source source;
/// auto token = source.get_token();
/// 
/// // Pass token to cancellable operations
/// auto task = do_work(token);
/// 
/// // Later, cancel the operation
/// source.cancel();
/// ```
class cancel_source {
public:
    /// Create a new cancel source
    cancel_source() 
        : state_(std::make_shared<detail::cancel_state>()) {}
    
    /// Get a token associated with this source
    cancel_token get_token() const noexcept {
        return cancel_token{state_};
    }
    
    /// Request cancellation
    /// All registered callbacks will be invoked and all tokens will report
    /// is_cancelled() == true
    void cancel() {
        if (state_) {
            state_->trigger();
        }
    }
    
    /// Check if cancellation has been requested
    bool is_cancelled() const noexcept {
        return state_ && state_->cancelled.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<detail::cancel_state> state_;
};

} // namespace elio::coro
