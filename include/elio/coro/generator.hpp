#pragma once

#include "promise_base.hpp"
#include "vthread_stack.hpp"
#include <coroutine>
#include <optional>
#include <exception>
#include <utility>
#include <type_traits>

namespace elio::coro {

/// Macro for range-for-like async generator iteration (zero overhead).
/// Supports break and continue naturally.
///
/// Usage:
///     auto gen = produce_values();
///     ELIO_CO_FOR(v, gen) {
///         std::cout << v << "\n";
///         if (some_condition) break;
///     }
#define ELIO_CO_FOR(var, gen)                                       \
    while (auto _elio_gen_val_ = co_await (gen).next())             \
        if (auto& var = *_elio_gen_val_; true)

namespace detail {

/// Minimal coroutine type for generator::for_each implementation.
/// Acts as both the coroutine return type and an awaitable.
struct for_each_task {
    struct promise_type {
        std::exception_ptr exception_;
        std::coroutine_handle<> consumer_;

        for_each_task get_return_object() noexcept {
            return for_each_task{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        auto final_suspend() noexcept {
            struct final_awaiter {
                [[nodiscard]] bool await_ready() const noexcept { return false; }
                [[nodiscard]] std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> h) noexcept {
                    return h.promise().consumer_;
                }
                void await_resume() const noexcept {}
            };
            return final_awaiter{};
        }

        void return_void() noexcept {}

        void unhandled_exception() noexcept {
            exception_ = std::current_exception();
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;
    handle_type handle_;

    for_each_task(handle_type h) noexcept : handle_(h) {}

    for_each_task(for_each_task&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    for_each_task(const for_each_task&) = delete;
    for_each_task& operator=(const for_each_task&) = delete;
    for_each_task& operator=(for_each_task&&) = delete;

    ~for_each_task() {
        if (handle_) handle_.destroy();
    }

    // Awaitable interface
    [[nodiscard]] bool await_ready() const noexcept {
        return !handle_ || handle_.done();
    }

    [[nodiscard]] std::coroutine_handle<> await_suspend(
        std::coroutine_handle<> consumer) noexcept {
        handle_.promise().consumer_ = consumer;
        return handle_;  // symmetric transfer to the loop coroutine
    }

    void await_resume() {
        auto h = std::exchange(handle_, nullptr);
        auto ex = h.promise().exception_;
        h.destroy();
        if (ex) std::rethrow_exception(ex);
    }
};

} // namespace detail

/// Async generator with symmetric-transfer scheduling.
///
/// A single type that serves as both the coroutine return type and the
/// consumer interface.  The consumer drives the producer via symmetric
/// transfer: each `co_await gen.next()` resumes the producer, and each
/// `co_yield` transfers control back to the consumer.
///
/// Because the producer is always suspended at a known point when the
/// consumer has control, the generator can safely destroy the producer
/// handle in its destructor — no cancellation flags, no idle tracking,
/// no scheduling races.
///
/// If the producer contains internal co_await (e.g. async I/O), the
/// awaitable suspends the producer normally; the I/O system resumes it
/// later, and it eventually co_yield's back to the consumer.
///
/// Example:
/// ```cpp
/// generator<int> produce_values() {
///     for (int i = 0; i < 10; ++i) {
///         co_yield i;
///     }
/// }
///
/// task<void> consume() {
///     auto gen = produce_values();
///
///     // Option 1: while loop
///     while (auto val = co_await gen.next()) {
///         std::cout << *val << "\n";
///     }
///
///     // Option 2: ELIO_CO_FOR macro (range-for-like, zero overhead)
///     ELIO_CO_FOR(v, gen) {
///         std::cout << v << "\n";
///     }
///
///     // Option 3: for_each method (functional style)
///     co_await gen.for_each([](int v) {
///         std::cout << v << "\n";
///     });
/// }
/// ```
template<typename T>
class generator {
public:
    class promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

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

        [[nodiscard]] generator get_return_object() noexcept {
            return generator{handle_type::from_promise(*this)};
        }

        // Suspend initially and detach from parent frame chain so that
        // the producer does not pollute the caller's debug frame stack.
        [[nodiscard]] auto initial_suspend() noexcept {
            struct initial_awaiter {
                promise_type* self;

                [[nodiscard]] bool await_ready() const noexcept { return false; }

                void await_suspend(std::coroutine_handle<>) noexcept {
                    self->detach_from_parent();
                }

                void await_resume() noexcept {}
            };
            return initial_awaiter{this};
        }

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

    generator() = default;

    explicit generator(handle_type h) noexcept : handle_(h) {}

    generator(generator&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    generator& operator=(generator&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    generator(const generator&) = delete;
    generator& operator=(const generator&) = delete;

    ~generator() {
        if (handle_) {
            handle_.destroy();
        }
    }

    /// Get the next value.  Returns std::nullopt when the producer is done.
    auto next() {
        struct next_awaiter {
            handle_type producer;

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
        return next_awaiter{handle_};
    }

    /// Iterate over all values, calling func for each one.
    ///
    /// If func returns bool, returning false stops iteration (early break).
    /// If func returns void, iterates until the generator is exhausted.
    ///
    /// Usage:
    ///     co_await gen.for_each([](int v) {
    ///         std::cout << v << "\n";
    ///     });
    ///
    ///     // With early termination:
    ///     co_await gen.for_each([](int v) -> bool {
    ///         std::cout << v << "\n";
    ///         return v < 5;  // stop when v >= 5
    ///     });
    template<typename F>
    auto for_each(F&& func) {
        auto loop = [](generator& gen, std::decay_t<F> f) -> detail::for_each_task {
            while (auto val = co_await gen.next()) {
                using R = decltype(f(std::move(*val)));
                if constexpr (std::is_same_v<R, bool>) {
                    if (!f(std::move(*val))) break;
                } else {
                    f(std::move(*val));
                }
            }
        };
        return loop(*this, std::forward<F>(func));
    }

    [[nodiscard]] bool finished() const noexcept {
        return !handle_ || handle_.done();
    }

private:
    handle_type handle_{nullptr};
};

} // namespace elio::coro
