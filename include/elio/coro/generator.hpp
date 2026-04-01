#pragma once

#include "promise_base.hpp"
#include "vthread_stack.hpp"
#include <coroutine>
#include <optional>
#include <exception>
#include <utility>
#include <type_traits>

namespace elio::coro {

/// Forward declaration
template<typename T>
class generator;

/// Producer for generator — returned by coroutines that use co_yield.
///
/// The producer coroutine is driven by the consumer via symmetric transfer:
/// - Consumer calls co_await gen.next() → resumes producer
/// - Producer hits co_yield → stores value, resumes consumer
/// - Producer falls off the end → resumes consumer with done()=true
///
/// This avoids independent scheduling and keeps lifecycle management simple:
/// the generator object owns the producer handle and destroys it when done.
template<typename T>
class generator_producer {
public:
    class promise_type : public promise_base {
    public:
        std::optional<T> value_;
        std::exception_ptr exception_;
        std::coroutine_handle<> consumer_;

        // Use global heap for producer frames (not vstack)
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

        // Producer starts suspended; first next() call begins execution.
        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }

        // At end of producer: resume consumer so it sees done()=true.
        [[nodiscard]] auto final_suspend() noexcept {
            struct final_awaiter {
                [[nodiscard]] bool await_ready() const noexcept { return false; }

                [[nodiscard]] std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> h) noexcept {
                    auto consumer = h.promise().consumer_;
                    if (consumer) {
                        return consumer;  // symmetric transfer → consumer
                    }
                    return std::noop_coroutine();
                }

                void await_resume() const noexcept {}
            };
            return final_awaiter{};
        }

        // co_yield value: store value, symmetric transfer to consumer.
        template<typename U = T>
        auto yield_value(U&& value) {
            value_.emplace(std::forward<U>(value));

            struct yield_awaiter {
                std::coroutine_handle<> consumer;

                [[nodiscard]] bool await_ready() const noexcept { return false; }

                [[nodiscard]] std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<>) noexcept {
                    return consumer;  // symmetric transfer → consumer
                }

                void await_resume() noexcept {}
            };
            return yield_awaiter{consumer_};
        }

        void return_void() noexcept {}

        void unhandled_exception() noexcept {
            promise_base::unhandled_exception();
            exception_ = std::current_exception();
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
        return std::exchange(handle_, nullptr);
    }
};

/// Async generator with symmetric-transfer scheduling.
///
/// The consumer drives the producer: each co_await gen.next() resumes the
/// producer coroutine via symmetric transfer.  The producer runs until it
/// co_yield's (transferring back) or finishes (transferring back with
/// done()=true).  Because the producer is always suspended at a known
/// point when the consumer has control, the generator can safely destroy
/// the producer handle in its destructor — no cancellation flags, no
/// idle tracking, no scheduling races.
///
/// If the producer contains internal co_await (e.g. async I/O), the
/// awaitable suspends the producer normally; the I/O system resumes it
/// later, and it eventually co_yield's back to the consumer.
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
    using producer_handle_type = typename producer_type::handle_type;
    using promise_type = typename producer_type::promise_type;

    generator() = default;

    explicit generator(producer_type producer)
        : producer_handle_(producer.release())
    {
        if (producer_handle_) {
            producer_handle_.promise().detach_from_parent();
        }
    }

    generator(generator&& other) noexcept
        : producer_handle_(std::exchange(other.producer_handle_, nullptr)) {}

    generator& operator=(generator&& other) noexcept {
        if (this != &other) {
            if (producer_handle_) {
                producer_handle_.destroy();
            }
            producer_handle_ = std::exchange(other.producer_handle_, nullptr);
        }
        return *this;
    }

    generator(const generator&) = delete;
    generator& operator=(const generator&) = delete;

    ~generator() {
        if (producer_handle_) {
            producer_handle_.destroy();
        }
    }

    /// Get the next value.  Returns std::nullopt when the producer is done.
    auto next() {
        struct next_awaiter {
            producer_handle_type producer;

            [[nodiscard]] bool await_ready() const noexcept {
                // If producer already done (or null), no need to suspend.
                return !producer || producer.done();
            }

            [[nodiscard]] std::coroutine_handle<> await_suspend(
                std::coroutine_handle<> consumer) noexcept {
                // Tell the producer who to transfer back to.
                producer.promise().consumer_ = consumer;
                // Symmetric transfer → producer runs until co_yield / end.
                return producer;
            }

            std::optional<T> await_resume() {
                if (!producer || producer.done()) {
                    // Producer finished — check for unhandled exception.
                    if (producer && producer.promise().exception_) {
                        std::rethrow_exception(producer.promise().exception_);
                    }
                    return std::nullopt;
                }
                // Producer yielded a value.
                auto val = std::move(producer.promise().value_);
                producer.promise().value_.reset();
                return val;
            }
        };
        return next_awaiter{producer_handle_};
    }

    [[nodiscard]] bool finished() const noexcept {
        return !producer_handle_ || producer_handle_.done();
    }

private:
    producer_handle_type producer_handle_{nullptr};
};

} // namespace elio::coro
