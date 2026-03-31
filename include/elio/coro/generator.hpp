#pragma once

#include "promise_base.hpp"
#include "vthread_stack.hpp"
#include <coroutine>
#include <optional>
#include <exception>
#include <utility>
#include <type_traits>
#include <atomic>
#include <memory>

namespace elio::runtime {
class scheduler;
scheduler* get_current_scheduler() noexcept;
void schedule_handle(std::coroutine_handle<> handle) noexcept;
} // namespace elio::runtime

namespace elio::coro {

namespace detail {

/// Shared state between generator producer and consumer.
template<typename T>
struct gen_state {
    std::optional<T> current_value_;
    std::exception_ptr exception_;
    
    // Producer handle for resuming after consumer takes value
    std::coroutine_handle<> producer_handle_;
    
    // Self-reference for safe lifetime management in awaiter
    std::weak_ptr<gen_state> self_;
    
    // Hot path: alignment to avoid false sharing
    alignas(64) std::atomic<std::coroutine_handle<>> waiting_consumer_{nullptr};
    std::atomic<bool> finished_{false};
    std::atomic<bool> value_ready_{false};

    /// Called by producer when yielding a value.
    template<typename U>
    void yield_value(U&& value) {
        current_value_.emplace(std::forward<U>(value));
        value_ready_.store(true, std::memory_order_release);
        
        // Wake consumer if waiting
        auto consumer = waiting_consumer_.exchange(nullptr, std::memory_order_acq_rel);
        if (consumer) {
            runtime::schedule_handle(consumer);
        }
    }

    /// Called by producer when done.
    void complete() {
        finished_.store(true, std::memory_order_release);
        
        auto consumer = waiting_consumer_.exchange(nullptr, std::memory_order_acq_rel);
        if (consumer) {
            runtime::schedule_handle(consumer);
        }
    }

    /// Called by producer on exception.
    void set_exception(std::exception_ptr ex) {
        exception_ = std::move(ex);
        finished_.store(true, std::memory_order_release);
        
        auto consumer = waiting_consumer_.exchange(nullptr, std::memory_order_acq_rel);
        if (consumer) {
            runtime::schedule_handle(consumer);
        }
    }

    /// Called by consumer to get next value.
    auto next() {
        struct next_awaiter {
            std::weak_ptr<gen_state> weak_state;

            bool await_ready() const noexcept {
                auto state = weak_state.lock();
                if (!state) return true;  // State destroyed, return immediately
                return state->value_ready_.load(std::memory_order_acquire) ||
                       state->finished_.load(std::memory_order_acquire);
            }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> consumer) {
                auto state = weak_state.lock();
                if (!state) {
                    return std::noop_coroutine();
                }
                
                // Try to register as waiting consumer
                std::coroutine_handle<> expected = nullptr;
                if (state->waiting_consumer_.compare_exchange_strong(expected, consumer,
                        std::memory_order_release, std::memory_order_acquire)) {
                    // Double-check for value or finish (race condition)
                    if (state->value_ready_.load(std::memory_order_acquire) ||
                        state->finished_.load(std::memory_order_acquire)) {
                        auto reclaimed = state->waiting_consumer_.exchange(nullptr, std::memory_order_acq_rel);
                        if (reclaimed) {
                            runtime::schedule_handle(consumer);
                        }
                    }
                    return std::noop_coroutine();
                }
                return std::noop_coroutine();
            }

            std::optional<T> await_resume() {
                auto state = weak_state.lock();
                if (!state) {
                    return std::nullopt;
                }
                
                if (state->exception_) {
                    std::rethrow_exception(state->exception_);
                }
                
                if (state->finished_.load(std::memory_order_acquire) &&
                    !state->value_ready_.load(std::memory_order_acquire)) {
                    return std::nullopt;
                }
                
                if (state->value_ready_.load(std::memory_order_acquire)) {
                    auto val = std::move(state->current_value_);
                    state->current_value_.reset();
                    state->value_ready_.store(false, std::memory_order_release);
                    
                    // Resume producer to generate next value
                    if (state->producer_handle_ && !state->finished_.load(std::memory_order_acquire)) {
                        runtime::schedule_handle(state->producer_handle_);
                    }
                    
                    return val;
                }
                return std::nullopt;
            }
        };

        return next_awaiter{self_};
    }
};

} // namespace detail

/// Forward declaration
template<typename T>
class generator;

/// Producer for generator.
template<typename T>
class generator_producer {
public:
    class promise_type : public promise_base {
    public:
        std::shared_ptr<detail::gen_state<T>> state_;
        std::coroutine_handle<> continuation_;
        bool detached_ = false;

        // Use global heap for producer frames to avoid vstack ownership issues
        void* operator new(size_t size) {
            return ::operator new(size);
        }
        void operator delete(void* ptr, size_t) noexcept {
            ::operator delete(ptr);
        }

        [[nodiscard]] generator_producer get_return_object() noexcept {
            return generator_producer{
                std::coroutine_handle<promise_type>::from_promise(*this)
            };
        }

        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }

        [[nodiscard]] auto final_suspend() noexcept {
            struct final_awaiter {
                [[nodiscard]] bool await_ready() const noexcept { return false; }

                [[nodiscard]] std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> h) noexcept {
                    if (h.promise().state_) {
                        h.promise().state_->complete();
                    }
                    
                    auto continuation = h.promise().continuation_;
                    if (continuation) {
                        return continuation;
                    } else if (h.promise().detached_) {
                        auto* vstack_to_delete = h.promise().release_vstack_ownership();
                        h.destroy();
                        delete vstack_to_delete;
                        return std::noop_coroutine();
                    }
                    return std::noop_coroutine();
                }

                void await_resume() const noexcept {}
            };
            return final_awaiter{};
        }

        template<typename U = T>
        std::suspend_always yield_value(U&& value) {
            if (state_) {
                state_->yield_value(std::forward<U>(value));
            }
            return {};  // Always suspend after yield
        }

        void return_void() noexcept {
            if (state_) {
                state_->complete();
            }
        }

        void unhandled_exception() noexcept {
            promise_base::unhandled_exception();
            if (state_) {
                state_->set_exception(std::current_exception());
            }
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit generator_producer(handle_type h) noexcept : handle_(h) {}
    
    generator_producer(generator_producer&& other) noexcept 
        : handle_(std::exchange(other.handle_, nullptr)) {}
    
    generator_producer& operator=(generator_producer&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    generator_producer(const generator_producer&) = delete;
    generator_producer& operator=(const generator_producer&) = delete;

    ~generator_producer() {
        if (handle_) handle_.destroy();
    }

private:
    friend class generator<T>;
    handle_type handle_;

    handle_type release() noexcept {
        if (handle_) {
            handle_.promise().detached_ = true;
        }
        return std::exchange(handle_, nullptr);
    }
};

/// Generator with scheduler integration.
///
/// Uses a producer-consumer model where the producer coroutine generates
/// values asynchronously and the consumer retrieves them via co_await next().
///
/// Example:
/// ```cpp
/// generator_producer<int> produce_values() {
///     for (int i = 0; i < 10; ++i) {
///         co_yield i;
///     }
/// }
///
/// task<void> consume() {
///     generator<int> gen(produce_values());
///     while (auto val = co_await gen.next()) {
///         std::cout << *val << "\n";
///     }
/// }
/// ```
template<typename T>
class generator {
public:
    using value_type = T;
    using producer_type = generator_producer<T>;

    generator() : state_(std::make_shared<detail::gen_state<T>>()) {
        state_->self_ = state_;
    }
    
    explicit generator(producer_type producer)
        : state_(std::make_shared<detail::gen_state<T>>()) 
        , producer_handle_(producer.release()) 
    {
        state_->self_ = state_;
        if (producer_handle_) {
            producer_handle_.promise().state_ = state_;
            producer_handle_.promise().detached_ = true;
            // Store producer handle for resumption
            state_->producer_handle_ = producer_handle_;
            // Detach from current thread's frame chain
            producer_handle_.promise().detach_from_parent();
            // Start the producer
            runtime::schedule_handle(producer_handle_);
        }
    }

    generator(generator&& other) noexcept
        : state_(std::move(other.state_))
        , producer_handle_(std::move(other.producer_handle_))
    {
        other.state_ = std::make_shared<detail::gen_state<T>>();
        other.state_->self_ = other.state_;
    }

    generator& operator=(generator&& other) noexcept {
        if (this != &other) {
            state_ = std::move(other.state_);
            producer_handle_ = std::move(other.producer_handle_);
            other.state_ = std::make_shared<detail::gen_state<T>>();
            other.state_->self_ = other.state_;
        }
        return *this;
    }

    generator(const generator&) = delete;
    generator& operator=(const generator&) = delete;

    /// Get the next value. Returns std::nullopt when finished.
    auto next() {
        return state_->next();
    }

    [[nodiscard]] bool finished() const noexcept {
        return state_ && state_->finished_.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<detail::gen_state<T>> state_;
    typename producer_type::handle_type producer_handle_;
};

} // namespace elio::coro
