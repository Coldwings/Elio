#pragma once

#include "promise_base.hpp"
#include "cancel_token.hpp"
#include <coroutine>
#include <optional>
#include <exception>
#include <utility>
#include <type_traits>
#include <atomic>
#include <memory>
#include <string>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace elio::runtime {
class scheduler;
scheduler* get_current_scheduler() noexcept;
void schedule_handle(std::coroutine_handle<> handle) noexcept;
}

namespace elio::coro {

/// Task execution status
enum class task_status {
    pending,       ///< Not started or currently executing
    completed,     ///< Normal completion (success)
    logic_failed,  ///< Business failure (explicit failure, not exception)
    exception,     ///< Exception failure (exception thrown)
    cancelled      ///< Cancelled
};

/// Task result status (for task_result / awaitable_result)
enum class result_status {
    completed,     ///< Normal completion
    logic_failed,  ///< Business failure
    timeout,       ///< Timeout
    cancelled,     ///< Cancelled
    exception      ///< Exception failure
};

/// Failure info (business failure, not exception)
struct failure {
    int code = 0;              ///< Error code
    std::string message;       ///< Error message
    
    failure() = default;
    failure(int c, std::string msg) : code(c), message(std::move(msg)) {}
    
    explicit failure(std::string msg) : code(0), message(std::move(msg)) {}
};

/// Helper function: create failure (for co_return, only for non-void task)
/// Usage: co_return coro::fail(404, "not found");
inline failure fail(int code, std::string message) {
    return failure{code, std::move(message)};
}

inline failure fail(std::string message) {
    return failure{0, std::move(message)};
}

namespace detail {

/// Internal shared state
template<typename T>
struct task_state {
    // State and result storage
    std::atomic<task_status> status_{task_status::pending};
    std::optional<T> value_;
    failure failure_;
    std::exception_ptr exception_;
    
    // Waiter management
    std::atomic<void*> waiter_{nullptr};
    std::mutex mutex_;
    
    // Cancel control
    std::atomic<bool> cancel_requested_{false};
    
    void set_value(T&& val) {
        std::lock_guard<std::mutex> lock(mutex_);
        value_.emplace(std::move(val));
        status_.store(task_status::completed, std::memory_order_release);
        notify_waiter();
    }
    
    void set_failure(failure f) {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_ = std::move(f);
        status_.store(task_status::logic_failed, std::memory_order_release);
        notify_waiter();
    }
    
    void set_exception(std::exception_ptr ex) {
        std::lock_guard<std::mutex> lock(mutex_);
        exception_ = ex;
        status_.store(task_status::exception, std::memory_order_release);
        notify_waiter();
    }
    
    void set_cancelled() {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.store(task_status::cancelled, std::memory_order_release);
        notify_waiter();
    }
    
    void notify_waiter() {
        void* waiter_addr = waiter_.exchange(nullptr, std::memory_order_acq_rel);
        if (waiter_addr) {
            auto waiter = std::coroutine_handle<>::from_address(waiter_addr);
            runtime::schedule_handle(waiter);
        }
    }
    
    [[nodiscard]] bool is_done() const noexcept {
        auto s = status_.load(std::memory_order_acquire);
        return s == task_status::completed ||
               s == task_status::logic_failed ||
               s == task_status::exception ||
               s == task_status::cancelled;
    }
    
    bool set_waiter(std::coroutine_handle<> h) noexcept {
        // Fast path: check if already done before trying to set waiter
        // This avoids the race of setting a waiter on an already-completed task
        if (is_done()) {
            return false;
        }

        // Try to atomically set the waiter
        void* expected = nullptr;
        if (waiter_.compare_exchange_strong(expected, h.address(),
                std::memory_order_release, std::memory_order_acquire)) {
            // Double-check if task completed between our initial check and CAS
            // If so, we need to notify the waiter we just set
            if (is_done()) {
                void* addr = waiter_.exchange(nullptr, std::memory_order_acq_rel);
                if (addr) {
                    runtime::schedule_handle(std::coroutine_handle<>::from_address(addr));
                }
                return false;
            }
            return true;
        }
        // Another waiter already set (shouldn't happen with single await)
        return false;
    }
    
    void request_cancel() {
        cancel_requested_.store(true, std::memory_order_release);
    }
    
    [[nodiscard]] bool is_cancellation_requested() const noexcept {
        return cancel_requested_.load(std::memory_order_acquire);
    }
};

/// void specialization
template<>
struct task_state<void> {
    std::atomic<task_status> status_{task_status::pending};
    failure failure_;
    std::exception_ptr exception_;
    std::atomic<void*> waiter_{nullptr};
    std::mutex mutex_;
    std::atomic<bool> cancel_requested_{false};
    
    void set_value() {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.store(task_status::completed, std::memory_order_release);
        notify_waiter();
    }
    
    void set_failure(failure f) {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_ = std::move(f);
        status_.store(task_status::logic_failed, std::memory_order_release);
        notify_waiter();
    }
    
    void set_exception(std::exception_ptr ex) {
        std::lock_guard<std::mutex> lock(mutex_);
        exception_ = ex;
        status_.store(task_status::exception, std::memory_order_release);
        notify_waiter();
    }
    
    void set_cancelled() {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.store(task_status::cancelled, std::memory_order_release);
        notify_waiter();
    }
    
    void notify_waiter() {
        void* waiter_addr = waiter_.exchange(nullptr, std::memory_order_acq_rel);
        if (waiter_addr) {
            auto waiter = std::coroutine_handle<>::from_address(waiter_addr);
            runtime::schedule_handle(waiter);
        }
    }
    
    [[nodiscard]] bool is_done() const noexcept {
        auto s = status_.load(std::memory_order_acquire);
        return s == task_status::completed ||
               s == task_status::logic_failed ||
               s == task_status::exception ||
               s == task_status::cancelled;
    }
    
    bool set_waiter(std::coroutine_handle<> h) noexcept {
        // Fast path: check if already done before trying to set waiter
        // This avoids the race of setting a waiter on an already-completed task
        if (is_done()) {
            return false;
        }

        // Try to atomically set the waiter
        void* expected = nullptr;
        if (waiter_.compare_exchange_strong(expected, h.address(),
                std::memory_order_release, std::memory_order_acquire)) {
            // Double-check if task completed between our initial check and CAS
            // If so, we need to notify the waiter we just set
            if (is_done()) {
                void* addr = waiter_.exchange(nullptr, std::memory_order_acq_rel);
                if (addr) {
                    runtime::schedule_handle(std::coroutine_handle<>::from_address(addr));
                }
                return false;
            }
            return true;
        }
        // Another waiter already set (shouldn't happen with single await)
        return false;
    }
    
    void request_cancel() {
        cancel_requested_.store(true, std::memory_order_release);
    }
    
    [[nodiscard]] bool is_cancellation_requested() const noexcept {
        return cancel_requested_.load(std::memory_order_acquire);
    }
};

} // namespace detail

// ============================================================================
// task_result<T> - Result wrapper
// ============================================================================

template<typename T>
class task_result {
public:
    using value_type = T;
    
    task_result() = default;
    
    /// Construct success result
    explicit task_result(T value)
        : status_(result_status::completed)
        , value_(std::move(value)) {}
    
    /// Construct business failure result
    explicit task_result(result_status status, failure f)
        : status_(status)
        , failure_(std::move(f)) {}
    
    /// Construct exception result
    explicit task_result(result_status status, std::exception_ptr ep)
        : status_(status)
        , exception_(std::move(ep)) {}
    
    /// Construct timeout/cancelled result
    explicit task_result(result_status status)
        : status_(status) {}
    
    // Move semantics
    task_result(task_result&&) = default;
    task_result& operator=(task_result&&) = default;
    
    // No copy support
    task_result(const task_result&) = delete;
    task_result& operator=(const task_result&) = delete;
    
    // ===== Status query =====
    [[nodiscard]] bool has_value() const noexcept {
        return status_ == result_status::completed;
    }
    
    [[nodiscard]] bool has_failure() const noexcept {
        return status_ == result_status::logic_failed;
    }
    
    [[nodiscard]] bool has_exception() const noexcept {
        return status_ == result_status::exception;
    }
    
    [[nodiscard]] result_status status() const noexcept {
        return status_;
    }
    
    [[nodiscard]] bool is_timeout() const noexcept {
        return status_ == result_status::timeout;
    }
    
    [[nodiscard]] bool is_cancelled() const noexcept {
        return status_ == result_status::cancelled;
    }
    
    [[nodiscard]] bool is_logic_failed() const noexcept {
        return status_ == result_status::logic_failed;
    }
    
    // ===== Value access =====
    T& value() & {
        return *value_;
    }
    
    T&& value() && {
        return std::move(*value_);
    }
    
    const T& value() const & {
        return *value_;
    }
    
    template<typename U>
    T value_or(U&& default_value) const & {
        return has_value() ? value() : static_cast<T>(std::forward<U>(default_value));
    }
    
    template<typename U>
    T value_or(U&& default_value) && {
        return has_value() ? std::move(value()) : static_cast<T>(std::forward<U>(default_value));
    }
    
    // ===== Result access =====
    const failure& failure_info() const {
        return failure_;
    }
    
    std::exception_ptr exception() const {
        return exception_;
    }
    
    std::string error_message() const {
        if (has_failure()) {
            return failure_.message;
        }
        if (!exception_) return {};
        try {
            std::rethrow_exception(exception_);
        } catch (const std::exception& e) {
            return e.what();
        } catch (...) {
            return "unknown exception";
        }
    }
    
    // ===== Implicit conversion =====
    explicit operator bool() const noexcept {
        return has_value();
    }
    
private:
    result_status status_ = result_status::exception;
    std::optional<T> value_;
    failure failure_;
    std::exception_ptr exception_;
};

// ===== void specialization =====
template<>
class task_result<void> {
public:
    using value_type = void;
    
    task_result() = default;
    
    /// Construct success/timeout/cancelled result
    explicit task_result(result_status status)
        : status_(status) {}
    
    /// Construct business failure result
    explicit task_result(result_status status, failure f)
        : status_(status)
        , failure_(std::move(f)) {}
    
    /// Construct exception result
    explicit task_result(result_status status, std::exception_ptr ep)
        : status_(status)
        , exception_(std::move(ep)) {}
    
    // Move semantics
    task_result(task_result&&) = default;
    task_result& operator=(task_result&&) = default;
    
    task_result(const task_result&) = delete;
    task_result& operator=(const task_result&) = delete;
    
    // ===== Status query =====
    [[nodiscard]] bool has_value() const noexcept {
        return status_ == result_status::completed;
    }
    
    [[nodiscard]] bool has_failure() const noexcept {
        return status_ == result_status::logic_failed;
    }
    
    [[nodiscard]] bool has_exception() const noexcept {
        return status_ == result_status::exception;
    }
    
    [[nodiscard]] result_status status() const noexcept {
        return status_;
    }
    
    [[nodiscard]] bool is_timeout() const noexcept {
        return status_ == result_status::timeout;
    }
    
    [[nodiscard]] bool is_cancelled() const noexcept {
        return status_ == result_status::cancelled;
    }
    
    [[nodiscard]] bool is_logic_failed() const noexcept {
        return status_ == result_status::logic_failed;
    }
    
    // ===== Result access =====
    const failure& failure_info() const {
        return failure_;
    }
    
    std::exception_ptr exception() const {
        return exception_;
    }
    
    std::string error_message() const {
        if (has_failure()) {
            return failure_.message;
        }
        if (!exception_) return {};
        try {
            std::rethrow_exception(exception_);
        } catch (const std::exception& e) {
            return e.what();
        } catch (...) {
            return "unknown exception";
        }
    }
    
    explicit operator bool() const noexcept {
        return has_value();
    }
    
private:
    result_status status_ = result_status::exception;
    failure failure_;
    std::exception_ptr exception_;
};

// ============================================================================
// task_handle<T> - Task handle
// ============================================================================

template<typename T>
class task_handle {
public:
    using value_type = T;
    
    task_handle() = default;
    
    explicit task_handle(std::shared_ptr<detail::task_state<T>> state)
        : state_(std::move(state)) {}
    
    ~task_handle() = default;
    
    // Move semantics
    task_handle(task_handle&&) noexcept = default;
    task_handle& operator=(task_handle&&) noexcept = default;
    
    // No copy support
    task_handle(const task_handle&) = delete;
    task_handle& operator=(const task_handle&) = delete;
    
    // ===== Validity check =====
    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }
    
    explicit operator bool() const noexcept {
        return valid();
    }
    
    // ===== Status query =====
    [[nodiscard]] task_status status() const noexcept {
        if (!state_) return task_status::exception;
        return state_->status_.load(std::memory_order_acquire);
    }
    
    [[nodiscard]] bool is_done() const noexcept {
        if (!state_) return true;
        return state_->is_done();
    }
    
    [[nodiscard]] bool is_completed() const noexcept {
        return status() == task_status::completed;
    }
    
    [[nodiscard]] bool is_logic_failed() const noexcept {
        return status() == task_status::logic_failed;
    }
    
    [[nodiscard]] bool has_exception() const noexcept {
        return status() == task_status::exception;
    }
    
    [[nodiscard]] bool is_cancelled() const noexcept {
        return status() == task_status::cancelled;
    }
    
    [[nodiscard]] bool is_pending() const noexcept {
        return status() == task_status::pending;
    }
    
    // ===== Explicit result retrieval =====
    bool try_get(T& out) const {
        if (!state_) return false;
        std::lock_guard<std::mutex> lock(state_->mutex_);
        if (state_->status_.load(std::memory_order_relaxed) == task_status::completed) {
            if constexpr (!std::is_void_v<T>) {
                out = *state_->value_;
            }
            return true;
        }
        return false;
    }
    
    bool try_get(failure& out) const {
        if (!state_) return false;
        std::lock_guard<std::mutex> lock(state_->mutex_);
        if (state_->status_.load(std::memory_order_relaxed) == task_status::logic_failed) {
            out = state_->failure_;
            return true;
        }
        return false;
    }
    
    bool try_get(std::exception_ptr& out) const {
        if (!state_) return false;
        std::lock_guard<std::mutex> lock(state_->mutex_);
        if (state_->status_.load(std::memory_order_relaxed) == task_status::exception) {
            out = state_->exception_;
            return true;
        }
        return false;
    }
    
    template<typename U>
    T get_or(U&& default_value) const {
        T result;
        if (try_get(result)) {
            return result;
        }
        return static_cast<T>(std::forward<U>(default_value));
    }
    
    // ===== Get complete result =====
    task_result<T> get_result() const {
        if (!state_) {
            return task_result<T>(result_status::exception,
                std::make_exception_ptr(std::runtime_error("invalid handle")));
        }
        
        std::lock_guard<std::mutex> lock(state_->mutex_);
        auto s = state_->status_.load(std::memory_order_relaxed);
        
        switch (s) {
            case task_status::completed:
                if constexpr (std::is_void_v<T>) {
                    return task_result<T>(result_status::completed);
                } else {
                    return task_result<T>(*state_->value_);
                }
            case task_status::logic_failed:
                return task_result<T>(result_status::logic_failed, state_->failure_);
            case task_status::exception:
                return task_result<T>(result_status::exception, state_->exception_);
            case task_status::cancelled:
                return task_result<T>(result_status::cancelled);
            default:
                return task_result<T>(result_status::exception,
                    std::make_exception_ptr(std::runtime_error("task not done")));
        }
    }
    
    // ===== Synchronous wait =====
    task_status wait() {
        if (!state_) return task_status::exception;
        
        std::unique_lock<std::mutex> lock(state_->mutex_);
        while (!state_->is_done()) {
            lock.unlock();
            std::this_thread::yield();
            lock.lock();
        }
        return status();
    }
    
    template<typename Rep, typename Period>
    task_status wait_for(std::chrono::duration<Rep, Period> timeout) {
        if (!state_) return task_status::exception;
        
        auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lock(state_->mutex_);
        while (!state_->is_done()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return status();  // May still be pending
            }
            lock.unlock();
            std::this_thread::yield();
            lock.lock();
        }
        return status();
    }
    
    // ===== Cancel control =====
    void request_cancel() {
        if (!state_) return;
        state_->request_cancel();
    }
    
    [[nodiscard]] bool is_cancellation_requested() const noexcept {
        if (!state_) return false;
        return state_->is_cancellation_requested();
    }
    
    // ===== Coroutine await (returns task_result, no exception thrown) =====
    auto operator co_await() const {
        struct awaiter {
            std::shared_ptr<detail::task_state<T>> state;
            
            bool await_ready() const noexcept {
                if (!state) return true;
                return state->is_done();
            }
            
            bool await_suspend(std::coroutine_handle<> h) noexcept {
                if (!state) return false;
                return state->set_waiter(h);
            }
            
            task_result<T> await_resume() {
                if (!state) {
                    throw std::runtime_error("invalid handle");
                }
                
                std::lock_guard<std::mutex> lock(state->mutex_);
                auto s = state->status_.load(std::memory_order_relaxed);
                
                switch (s) {
                    case task_status::completed:
                        if constexpr (std::is_void_v<T>) {
                            return task_result<T>(result_status::completed);
                        } else {
                            return task_result<T>(std::move(*state->value_));
                        }
                    case task_status::logic_failed:
                        return task_result<T>(result_status::logic_failed, state->failure_);
                    case task_status::exception:
                        return task_result<T>(result_status::exception, state->exception_);
                    case task_status::cancelled:
                        return task_result<T>(result_status::cancelled);
                    default:
                        throw std::runtime_error("task not done");
                }
            }
        };
        return awaiter{state_};
    }
    
private:
    std::shared_ptr<detail::task_state<T>> state_;
};

// ============================================================================
// task_handle<void> - void specialization
// ============================================================================

template<>
class task_handle<void> {
public:
    using value_type = void;
    
    task_handle() = default;
    
    explicit task_handle(std::shared_ptr<detail::task_state<void>> state)
        : state_(std::move(state)) {}
    
    ~task_handle() = default;
    
    task_handle(task_handle&&) noexcept = default;
    task_handle& operator=(task_handle&&) noexcept = default;
    
    task_handle(const task_handle&) = delete;
    task_handle& operator=(const task_handle&) = delete;
    
    // ===== Validity check =====
    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }
    
    explicit operator bool() const noexcept {
        return valid();
    }
    
    // ===== Status query =====
    [[nodiscard]] task_status status() const noexcept {
        if (!state_) return task_status::exception;
        return state_->status_.load(std::memory_order_acquire);
    }
    
    [[nodiscard]] bool is_done() const noexcept {
        if (!state_) return true;
        return state_->is_done();
    }
    
    [[nodiscard]] bool is_completed() const noexcept {
        return status() == task_status::completed;
    }
    
    [[nodiscard]] bool is_logic_failed() const noexcept {
        return status() == task_status::logic_failed;
    }
    
    [[nodiscard]] bool has_exception() const noexcept {
        return status() == task_status::exception;
    }
    
    [[nodiscard]] bool is_cancelled() const noexcept {
        return status() == task_status::cancelled;
    }
    
    [[nodiscard]] bool is_pending() const noexcept {
        return status() == task_status::pending;
    }
    
    // ===== Explicit result retrieval =====
    bool try_get(failure& out) const {
        if (!state_) return false;
        std::lock_guard<std::mutex> lock(state_->mutex_);
        if (state_->status_.load(std::memory_order_relaxed) == task_status::logic_failed) {
            out = state_->failure_;
            return true;
        }
        return false;
    }
    
    bool try_get(std::exception_ptr& out) const {
        if (!state_) return false;
        std::lock_guard<std::mutex> lock(state_->mutex_);
        if (state_->status_.load(std::memory_order_relaxed) == task_status::exception) {
            out = state_->exception_;
            return true;
        }
        return false;
    }
    
    // ===== Get complete result =====
    task_result<void> get_result() const {
        if (!state_) {
            return task_result<void>(result_status::exception,
                std::make_exception_ptr(std::runtime_error("invalid handle")));
        }
        
        std::lock_guard<std::mutex> lock(state_->mutex_);
        auto s = state_->status_.load(std::memory_order_relaxed);
        
        switch (s) {
            case task_status::completed:
                return task_result<void>(result_status::completed);
            case task_status::logic_failed:
                return task_result<void>(result_status::logic_failed, state_->failure_);
            case task_status::exception:
                return task_result<void>(result_status::exception, state_->exception_);
            case task_status::cancelled:
                return task_result<void>(result_status::cancelled);
            default:
                return task_result<void>(result_status::exception,
                    std::make_exception_ptr(std::runtime_error("task not done")));
        }
    }
    
    // ===== Synchronous wait =====
    task_status wait() {
        if (!state_) return task_status::exception;
        
        std::unique_lock<std::mutex> lock(state_->mutex_);
        while (!state_->is_done()) {
            lock.unlock();
            std::this_thread::yield();
            lock.lock();
        }
        return status();
    }
    
    template<typename Rep, typename Period>
    task_status wait_for(std::chrono::duration<Rep, Period> timeout) {
        if (!state_) return task_status::exception;
        
        auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lock(state_->mutex_);
        while (!state_->is_done()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return status();
            }
            lock.unlock();
            std::this_thread::yield();
            lock.lock();
        }
        return status();
    }
    
    // ===== Cancel control =====
    void request_cancel() {
        if (!state_) return;
        state_->request_cancel();
    }
    
    [[nodiscard]] bool is_cancellation_requested() const noexcept {
        if (!state_) return false;
        return state_->is_cancellation_requested();
    }
    
    // ===== Coroutine await (returns task_result, no exception thrown) =====
    auto operator co_await() const {
        struct awaiter {
            std::shared_ptr<detail::task_state<void>> state;
            
            bool await_ready() const noexcept {
                if (!state) return true;
                return state->is_done();
            }
            
            bool await_suspend(std::coroutine_handle<> h) noexcept {
                if (!state) return false;
                return state->set_waiter(h);
            }
            
            task_result<void> await_resume() {
                if (!state) {
                    throw std::runtime_error("invalid handle");
                }
                
                std::lock_guard<std::mutex> lock(state->mutex_);
                auto s = state->status_.load(std::memory_order_relaxed);
                
                switch (s) {
                    case task_status::completed:
                        return task_result<void>(result_status::completed);
                    case task_status::logic_failed:
                        return task_result<void>(result_status::logic_failed, state->failure_);
                    case task_status::exception:
                        return task_result<void>(result_status::exception, state->exception_);
                    case task_status::cancelled:
                        return task_result<void>(result_status::cancelled);
                    default:
                        throw std::runtime_error("task not done");
                }
            }
        };
        return awaiter{state_};
    }
    
private:
    std::shared_ptr<detail::task_state<void>> state_;
};

} // namespace elio::coro
