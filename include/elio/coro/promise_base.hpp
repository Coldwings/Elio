#pragma once

#include <exception>

namespace elio::coro {

/// Base class for all coroutine promise types
/// Implements lightweight virtual stack tracking via thread-local intrusive list
class promise_base {
public:
    promise_base() noexcept : parent_(current_frame_) {
        current_frame_ = this;
    }
    
    ~promise_base() noexcept {
        current_frame_ = parent_;
    }

    promise_base(const promise_base&) = delete;
    promise_base& operator=(const promise_base&) = delete;
    promise_base(promise_base&&) = delete;
    promise_base& operator=(promise_base&&) = delete;

    void unhandled_exception() noexcept {
        exception_ = std::current_exception();
    }
    
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return exception_;
    }
    
    [[nodiscard]] promise_base* parent() const noexcept {
        return parent_;
    }

    [[nodiscard]] static promise_base* current_frame() noexcept {
        return current_frame_;
    }

private:
    promise_base* parent_;
    std::exception_ptr exception_;
    
    static inline thread_local promise_base* current_frame_ = nullptr;
};

} // namespace elio::coro
