#pragma once

#include "promise_base.hpp"
#include "frame_allocator.hpp"
#include "vthread_owner.hpp"
#include <coroutine>
#include <cstring>
#include <optional>
#include <exception>
#include <utility>
#include <type_traits>
#include <atomic>
#include <memory>

namespace elio::runtime {
class scheduler;  // Forward declaration
scheduler* get_current_scheduler() noexcept;
void schedule_handle(std::coroutine_handle<> handle) noexcept;
}

namespace elio::coro {

inline void* relocate_spawn_go_root_frame(void* source, size_t frame_size) {
    if (!source || frame_size == 0) return source;

    auto* owner = new vthread_owner();
    void* destination = owner->allocate(frame_size);
    if (!destination) {
        delete owner;
        return source;
    }

    std::memcpy(destination, source, frame_size);

    // GCC stores the get_return_object() return value (task<T>{h}) inside the
    // coroutine frame. That stored task contains a coroutine_handle whose
    // _M_fr_ptr == source (the old frame address). After memcpy this stale
    // self-reference remains, causing final_awaiter::await_suspend to receive
    // the OLD frame address via h.  Fix up every pointer-sized slot that still
    // holds the old address.
    auto* dst_bytes = static_cast<char*>(destination);
    const auto old_val = reinterpret_cast<uintptr_t>(source);
    const auto new_val = reinterpret_cast<uintptr_t>(destination);
    for (size_t i = 0; i + sizeof(void*) <= frame_size; i += sizeof(void*)) {
        uintptr_t slot;
        std::memcpy(&slot, dst_bytes + i, sizeof(slot));
        if (slot == old_val) {
            std::memcpy(dst_bytes + i, &new_val, sizeof(new_val));
        }
    }

    auto* destination_promise = promise_base::from_handle_address(destination);
    if (!destination_promise) {
        delete owner;
        return source;
    }

    destination_promise->set_vthread_owner(owner);
    destination_promise->set_activation_parent(nullptr);
    destination_promise->set_vthread_root(true);
    vthread_owner::mark_root_allocation(destination, true);
    return destination;
}

/// Free the backing allocation of a cold (pre-resume) coroutine frame
/// WITHOUT invoking C++ destructors.
///
/// Used after memcpy relocation: the "live" state has been copied to a new address;
/// 'ptr' is the abandoned source. Running destructors here would double-destroy
/// objects already owned by the relocated frame (e.g. captured task<T> handles,
/// shared_ptr ref counts would be incorrectly decremented).
inline void free_cold_frame_backing(void* ptr, size_t frame_size) noexcept {
    if (!ptr) return;
    // vthread_owner uses bump allocation; individual frees are no-ops.
    // Memory is reclaimed when the owning domain is destroyed.
    if (vthread_owner::inspect_allocation(ptr).found) {
        return;
    }
    // frame_allocator: return the block to the pool without running any destructor.
    frame_allocator::deallocate(ptr, frame_size);
}

template<typename T = void>
class task;

template<typename T = void>
class join_handle;

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

/// Shared state for join_handle<T> - stores result and waiter
template<typename T>
struct join_state {
    std::optional<T> value_;
    std::exception_ptr exception_;
    // waiter_/completed_ are the hot path: written by the coroutine producer
    // (complete()) and the awaiting consumer (set_waiter()).  Aligning them
    // to a new cache line separates them from value_/exception_ which may
    // be large and written only once, preventing false sharing.
    alignas(64) std::atomic<void*> waiter_{nullptr};  // Stores coroutine_handle address
    std::atomic<bool> completed_{false};
    
    void set_value(T&& value) {
        value_.emplace(std::move(value));
        complete();
    }
    
    void set_exception(std::exception_ptr ex) {
        exception_ = ex;
        complete();
    }
    
    void complete() {
        completed_.store(true, std::memory_order_release);
        void* waiter_addr = waiter_.exchange(nullptr, std::memory_order_acq_rel);
        if (waiter_addr) {
            auto waiter = std::coroutine_handle<>::from_address(waiter_addr);
            runtime::schedule_handle(waiter);
        }
    }
    
    [[nodiscard]] bool is_completed() const noexcept {
        return completed_.load(std::memory_order_acquire);
    }
    
    // Returns true if waiter was stored (should suspend), false if already completed
    bool set_waiter(std::coroutine_handle<> h) noexcept {
        void* expected = nullptr;
        if (waiter_.compare_exchange_strong(expected, h.address(), 
                std::memory_order_release, std::memory_order_acquire)) {
            // Check again if completed (race condition)
            if (completed_.load(std::memory_order_acquire)) {
                // Already completed, try to reclaim and resume
                void* addr = waiter_.exchange(nullptr, std::memory_order_acq_rel);
                if (addr) {
                    runtime::schedule_handle(std::coroutine_handle<>::from_address(addr));
                }
                return false;
            }
            return true;
        }
        return false;  // Already has a waiter (shouldn't happen with single await)
    }
    
    T get_value() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
        return std::move(*value_);
    }
};

/// Specialization for void
template<>
struct join_state<void> {
    std::exception_ptr exception_;
    // Hot atomics on their own cache line (see join_state<T> comment above)
    alignas(64) std::atomic<void*> waiter_{nullptr};
    std::atomic<bool> completed_{false};
    
    void set_value() {
        complete();
    }
    
    void set_exception(std::exception_ptr ex) {
        exception_ = ex;
        complete();
    }
    
    void complete() {
        completed_.store(true, std::memory_order_release);
        void* waiter_addr = waiter_.exchange(nullptr, std::memory_order_acq_rel);
        if (waiter_addr) {
            auto waiter = std::coroutine_handle<>::from_address(waiter_addr);
            runtime::schedule_handle(waiter);
        }
    }
    
    [[nodiscard]] bool is_completed() const noexcept {
        return completed_.load(std::memory_order_acquire);
    }
    
    bool set_waiter(std::coroutine_handle<> h) noexcept {
        void* expected = nullptr;
        if (waiter_.compare_exchange_strong(expected, h.address(),
                std::memory_order_release, std::memory_order_acquire)) {
            if (completed_.load(std::memory_order_acquire)) {
                void* addr = waiter_.exchange(nullptr, std::memory_order_acq_rel);
                if (addr) {
                    runtime::schedule_handle(std::coroutine_handle<>::from_address(addr));
                }
                return false;
            }
            return true;
        }
        return false;
    }
    
    void get_value() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }
};

} // namespace detail

/// Join handle for awaiting spawned tasks
/// Returned by task<T>::spawn(), allows co_await to get the result
template<typename T>
class join_handle {
public:
    explicit join_handle(std::shared_ptr<detail::join_state<T>> state) noexcept
        : state_(std::move(state)) {}
    
    join_handle(join_handle&&) noexcept = default;
    join_handle& operator=(join_handle&&) noexcept = default;
    
    join_handle(const join_handle&) = delete;
    join_handle& operator=(const join_handle&) = delete;
    
    [[nodiscard]] bool await_ready() const noexcept {
        return state_->is_completed();
    }

    bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
        // Keep a local copy of the shared_ptr to prevent use-after-free.
        // Without this, a race can occur:
        // 1. set_waiter() stores the awaiter and checks completed_
        // 2. Meanwhile, complete() is called, which schedules the awaiter
        // 3. The awaiter runs on another thread, finishes, and destroys this join_handle
        // 4. The last shared_ptr ref is gone, join_state is destroyed
        // 5. set_waiter() tries to access destroyed memory
        // Holding a local shared_ptr ensures join_state outlives set_waiter().
        auto state = state_;
        return state->set_waiter(awaiter);
    }

    T await_resume() {
        return state_->get_value();
    }
    
    /// Check if the spawned task has completed
    [[nodiscard]] bool is_ready() const noexcept {
        return state_->is_completed();
    }

private:
    std::shared_ptr<detail::join_state<T>> state_;
};

/// Specialization for void
template<>
class join_handle<void> {
public:
    explicit join_handle(std::shared_ptr<detail::join_state<void>> state) noexcept
        : state_(std::move(state)) {}
    
    join_handle(join_handle&&) noexcept = default;
    join_handle& operator=(join_handle&&) noexcept = default;
    
    join_handle(const join_handle&) = delete;
    join_handle& operator=(const join_handle&) = delete;
    
    [[nodiscard]] bool await_ready() const noexcept {
        return state_->is_completed();
    }

    bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
        // Keep a local copy of the shared_ptr to prevent use-after-free.
        // See join_handle<T>::await_suspend for detailed explanation.
        auto state = state_;
        return state->set_waiter(awaiter);
    }

    void await_resume() {
        state_->get_value();
    }

    [[nodiscard]] bool is_ready() const noexcept {
        return state_->is_completed();
    }

private:
    std::shared_ptr<detail::join_state<void>> state_;
};

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
            promise_base::set_next_frame_size(size);
            if (auto* owner = static_cast<::elio::coro::vthread_owner*>(promise_base::current_owner())) {
                if (void* ptr = owner->allocate(size)) {
                    return ptr;
                }
            }
            return frame_allocator::allocate(size);
        }
        
        void operator delete(void* ptr, size_t size) noexcept {
            auto owner_alloc = ::elio::coro::vthread_owner::inspect_allocation(ptr);
            if (owner_alloc.found) {
                if (owner_alloc.is_root) {
                    delete static_cast<::elio::coro::vthread_owner*>(owner_alloc.owner);
                }
                return;
            }

            auto frame_owner = frame_allocator::inspect_owner_metadata(ptr);
            if (frame_owner.found && frame_owner.is_root) {
                delete static_cast<::elio::coro::vthread_owner*>(frame_owner.owner);
            }
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
    
    /// Spawn this task on the current scheduler (fire-and-forget)
    /// The task will run asynchronously and self-destruct when complete
    void go() {
        if (handle_) {
            const size_t frame_size = handle_.promise().frame_size();
            void* old_ptr = handle_.address();
            void* relocated = relocate_spawn_go_root_frame(old_ptr, frame_size);
            if (relocated != old_ptr) {
                handle_ = handle_type::from_address(relocated);
                // Do NOT call old.destroy(): that would run destructors and
                // double-destroy captured objects now owned by the relocated frame.
                // Instead, free only the backing allocation.
                free_cold_frame_backing(old_ptr, frame_size);
            }
        }
        runtime::schedule_handle(release());
    }
    
    /// Spawn this task and return a join_handle for awaiting the result
    /// Usage: auto handle = some_task().spawn(); T result = co_await handle;
    [[nodiscard]] join_handle<T> spawn();

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter) noexcept {
        auto& promise = handle_.promise();
        auto* activation_parent = promise_base::from_handle_address(awaiter.address());
        promise.bind_activation_parent_once(activation_parent);

        void* owner = promise_base::current_owner();
        if (owner) {
            promise.bind_vthread_owner_once(owner);
        }
        promise.continuation_ = awaiter;
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
            promise_base::set_next_frame_size(size);
            if (auto* owner = static_cast<::elio::coro::vthread_owner*>(promise_base::current_owner())) {
                if (void* ptr = owner->allocate(size)) {
                    return ptr;
                }
            }
            return frame_allocator::allocate(size);
        }
        
        void operator delete(void* ptr, size_t size) noexcept {
            auto owner_alloc = ::elio::coro::vthread_owner::inspect_allocation(ptr);
            if (owner_alloc.found) {
                if (owner_alloc.is_root) {
                    delete static_cast<::elio::coro::vthread_owner*>(owner_alloc.owner);
                }
                return;
            }

            auto frame_owner = frame_allocator::inspect_owner_metadata(ptr);
            if (frame_owner.found && frame_owner.is_root) {
                delete static_cast<::elio::coro::vthread_owner*>(frame_owner.owner);
            }
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
    
    /// Spawn this task on the current scheduler (fire-and-forget)
    /// The task will run asynchronously and self-destruct when complete
    void go() {
        if (handle_) {
            const size_t frame_size = handle_.promise().frame_size();
            void* old_ptr = handle_.address();
            void* relocated = relocate_spawn_go_root_frame(old_ptr, frame_size);
            if (relocated != old_ptr) {
                handle_ = handle_type::from_address(relocated);
                // Do NOT call old.destroy(): that would run destructors and
                // double-destroy captured objects now owned by the relocated frame.
                // Instead, free only the backing allocation.
                free_cold_frame_backing(old_ptr, frame_size);
            }
        }
        runtime::schedule_handle(release());
    }
    
    /// Spawn this task and return a join_handle for awaiting completion
    /// Usage: auto handle = some_task().spawn(); co_await handle;
    [[nodiscard]] join_handle<void> spawn();

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter) noexcept {
        auto& promise = handle_.promise();
        auto* activation_parent = promise_base::from_handle_address(awaiter.address());
        promise.bind_activation_parent_once(activation_parent);

        void* owner = promise_base::current_owner();
        if (owner) {
            promise.bind_vthread_owner_once(owner);
        }
        promise.continuation_ = awaiter;
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

namespace detail {

/// Wrapper task that forwards result to join_state
template<typename T>
task<void> join_wrapper(task<T> t, std::shared_ptr<join_state<T>> state) {
    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(t);
            state->set_value();
        } else {
            T result = co_await std::move(t);
            state->set_value(std::move(result));
        }
    } catch (...) {
        state->set_exception(std::current_exception());
    }
}

} // namespace detail

// Out-of-line definitions for spawn() methods
template<typename T>
join_handle<T> task<T>::spawn() {
    auto state = std::make_shared<detail::join_state<T>>();
    detail::join_wrapper(std::move(*this), state).go();
    return join_handle<T>(std::move(state));
}

inline join_handle<void> task<void>::spawn() {
    auto state = std::make_shared<detail::join_state<void>>();
    detail::join_wrapper(std::move(*this), state).go();
    return join_handle<void>(std::move(state));
}

} // namespace elio::coro
