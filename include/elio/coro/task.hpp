#pragma once

#include "promise_base.hpp"
#include "frame_allocator.hpp"
#include <coroutine>
#include <optional>
#include <exception>
#include <utility>
#include <type_traits>

namespace elio::coro {

template<typename T = void>
class task;

namespace detail {

struct final_awaiter {
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    
    template<typename Promise>
    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
        auto continuation = h.promise().continuation_;
        if (continuation) {
            return continuation;
        } else if (h.promise().detached_) {
            // Detached task with no continuation - self-destruct
            h.destroy();
            return std::noop_coroutine();
        } else {
            // Owned task with no continuation - stay suspended for owner to destroy
            return std::noop_coroutine();
        }
    }
    
    void await_resume() const noexcept {}
};

} // namespace detail

/// Primary template for task<T> where T is not void
template<typename T>
class task {
public:
    struct promise_type : promise_base {
        std::optional<T> value_;
        std::coroutine_handle<> continuation_;
        bool detached_ = false;

        promise_type() noexcept = default;

        [[nodiscard]] task get_return_object() noexcept {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
        [[nodiscard]] detail::final_awaiter final_suspend() noexcept { return {}; }

        template<typename U>
        void return_value(U&& value) {
            value_.emplace(std::forward<U>(value));
        }

        // Custom allocator for coroutine frames
        void* operator new(size_t size) {
            return frame_allocator::allocate(size);
        }
        
        void operator delete(void* ptr, size_t size) noexcept {
            frame_allocator::deallocate(ptr, size);
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit task(handle_type handle) noexcept : handle_(handle) {}
    task(task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    task& operator=(task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    ~task() { if (handle_) handle_.destroy(); }

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    [[nodiscard]] handle_type handle() const noexcept { return handle_; }
    [[nodiscard]] handle_type release() noexcept { 
        if (handle_) handle_.promise().detached_ = true;
        return std::exchange(handle_, nullptr); 
    }

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter) noexcept {
        handle_.promise().continuation_ = awaiter;
        return handle_;
    }

    T await_resume() {
        auto& promise = handle_.promise();
        if (promise.exception()) {
            std::rethrow_exception(promise.exception());
        }
        return std::move(*promise.value_);
    }

private:
    handle_type handle_;
};

/// Specialization for task<void>
template<>
class task<void> {
public:
    struct promise_type : promise_base {
        std::coroutine_handle<> continuation_;
        bool detached_ = false;

        promise_type() noexcept = default;

        [[nodiscard]] task get_return_object() noexcept {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
        [[nodiscard]] detail::final_awaiter final_suspend() noexcept { return {}; }

        void return_void() noexcept {}

        // Custom allocator for coroutine frames
        void* operator new(size_t size) {
            return frame_allocator::allocate(size);
        }
        
        void operator delete(void* ptr, size_t size) noexcept {
            frame_allocator::deallocate(ptr, size);
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit task(handle_type handle) noexcept : handle_(handle) {}
    task(task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    task& operator=(task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    ~task() { if (handle_) handle_.destroy(); }

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    [[nodiscard]] handle_type handle() const noexcept { return handle_; }
    [[nodiscard]] handle_type release() noexcept { 
        if (handle_) handle_.promise().detached_ = true;
        return std::exchange(handle_, nullptr); 
    }

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter) noexcept {
        handle_.promise().continuation_ = awaiter;
        return handle_;
    }

    void await_resume() {
        auto& promise = handle_.promise();
        if (promise.exception()) {
            std::rethrow_exception(promise.exception());
        }
    }

private:
    handle_type handle_;
};

} // namespace elio::coro
