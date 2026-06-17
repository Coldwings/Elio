#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

namespace elio::runtime {
class scheduler;  // Forward declaration

// Defined in scheduler.hpp; cross-thread-safe handle dispatch that restores
// vthread_stack/promise frame context before resuming.
void schedule_handle(std::coroutine_handle<> handle) noexcept;
}

namespace elio::coro {

/// Result of a cancellable operation
enum class cancel_result {
    completed,   ///< Operation completed normally
    cancelled    ///< Operation was cancelled
};

namespace detail {

/// Type-erased callback node for the cancel_state intrusive list.
///
/// Each registration owns exactly one heap-allocated callback_node. The
/// callable lives in a small inline buffer when it fits (the typical case
/// for lambdas capturing 1-3 references); larger callables fall back to a
/// single secondary heap allocation. This eliminates the vector growth and
/// the per-callable std::function allocation of the previous design.
struct callback_node {
    callback_node* next = nullptr;
    uint64_t id = 0;

    static constexpr std::size_t inline_buf_size = 48;
    alignas(alignof(std::max_align_t)) std::byte buf[inline_buf_size];
    void* heap_ptr = nullptr;

    void (*invoke_fn)(callback_node*) = nullptr;
    void (*destroy_fn)(callback_node*) = nullptr;

    callback_node() = default;
    callback_node(const callback_node&) = delete;
    callback_node& operator=(const callback_node&) = delete;
    callback_node(callback_node&&) = delete;
    callback_node& operator=(callback_node&&) = delete;

    template<typename F>
    void emplace(F&& f) {
        using T = std::decay_t<F>;
        if constexpr (sizeof(T) <= inline_buf_size &&
                      alignof(T) <= alignof(std::max_align_t) &&
                      std::is_nothrow_move_constructible_v<T>) {
            ::new (static_cast<void*>(buf)) T(std::forward<F>(f));
            invoke_fn = [](callback_node* n) {
                (*std::launder(reinterpret_cast<T*>(n->buf)))();
            };
            destroy_fn = [](callback_node* n) {
                std::launder(reinterpret_cast<T*>(n->buf))->~T();
            };
        } else {
            heap_ptr = new T(std::forward<F>(f));
            invoke_fn = [](callback_node* n) {
                (*static_cast<T*>(n->heap_ptr))();
            };
            destroy_fn = [](callback_node* n) {
                delete static_cast<T*>(n->heap_ptr);
                n->heap_ptr = nullptr;
            };
        }
    }

    void invoke() {
        if (invoke_fn) invoke_fn(this);
    }

    ~callback_node() {
        if (destroy_fn) destroy_fn(this);
    }
};

/// Shared cancellation state (implementation detail).
///
/// Stores active callbacks as an intrusive singly-linked list of
/// callback_nodes guarded by a mutex. Compared with the previous
/// std::vector<std::pair<id, std::function>> implementation:
///   * registration is O(1) without vector growth/copies;
///   * each registration owns a single heap-allocated node, and its
///     small-buffer-optimized payload typically avoids the second
///     std::function allocation;
///   * unregistration is O(N) but performs no heap shuffling.
struct cancel_state {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    callback_node* head = nullptr;
    uint64_t next_id = 1;

    cancel_state() = default;
    cancel_state(const cancel_state&) = delete;
    cancel_state& operator=(const cancel_state&) = delete;

    template<typename F>
    uint64_t add_callback(F&& cb) {
        // Allocate the node before taking the lock to keep the critical
        // section short.
        auto* node = new callback_node;
        node->emplace(std::forward<F>(cb));

        std::unique_lock<std::mutex> lock(mutex);
        if (cancelled.load(std::memory_order_relaxed)) {
            // Already cancelled: invoke and drop without inserting.
            lock.unlock();
            node->invoke();
            delete node;
            return 0;
        }
        node->id = next_id++;
        node->next = head;
        head = node;
        return node->id;
    }

    void remove_callback(uint64_t id) {
        if (id == 0) return;
        callback_node* to_delete = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            callback_node** prev = &head;
            while (*prev) {
                if ((*prev)->id == id) {
                    to_delete = *prev;
                    *prev = (*prev)->next;
                    break;
                }
                prev = &(*prev)->next;
            }
        }
        delete to_delete;  // delete on nullptr is a no-op
    }

    void trigger() {
        callback_node* list = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (cancelled.exchange(true, std::memory_order_release)) {
                return;  // Already cancelled
            }
            list = head;
            head = nullptr;
        }
        // List is now exclusively owned by this call. Invoke each callback
        // outside the lock and free its node. Concurrent remove_callback()
        // calls will simply find no matching id in the (empty) head list,
        // which is the correct no-op semantics.
        while (list) {
            auto* next = list->next;
            list->invoke();
            delete list;
            list = next;
        }
    }

    ~cancel_state() {
        // Drop any callbacks that were never triggered or unregistered.
        while (head) {
            auto* next = head->next;
            delete head;
            head = next;
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
    ///
    /// **Concurrency note:** If the token is already cancelled, the callback
    /// is invoked synchronously in the calling thread. This invocation may
    /// overlap with callbacks still being dispatched by a concurrent
    /// `cancel()` call on another thread. Callbacks must not assume mutual
    /// exclusion with other cancel callbacks; use external synchronization
    /// if shared mutable state is accessed.
    ///
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
    ///
    /// **Deprecated.** This API is unsafe when ``handle`` is concurrently
    /// suspended on an ``io::io_awaitable_base`` subclass (recv/send/accept/
    /// connect/timeout/poll/read/write/close/...). Each io awaitable owns an
    /// ``op_state`` whose phase atomic is shared with an in-flight SQE. If
    /// cancellation fires while the SQE is still pending, two paths race to
    /// resume the same coroutine:
    ///
    ///   1. ``on_cancel_resume`` schedules the handle on a worker. The worker
    ///      resumes the coroutine; the io awaitable's destructor attempts to
    ///      CAS the op_state phase from ``pending`` to ``orphaned``.
    ///   2. The kernel CQE arrives concurrently. ``dispatch_op_state`` CASes
    ///      ``pending`` → ``completed`` (winning before the destructor runs)
    ///      and schedules the same handle a second time, resuming an
    ///      already-destroyed frame.
    ///
    /// The correct pattern is to give the io awaitable its own ``cancel_token``
    /// at construction (see ``time::sleep_for(duration, token)`` /
    /// ``cancellable_sleep_awaitable``). That implementation registers
    /// ``on_cancel`` on the token and, from the cancel callback, schedules a
    /// fire-and-forget coroutine onto the awaiter's worker that submits an
    /// ``IORING_OP_ASYNC_CANCEL`` SQE via ``io_context::cancel``. The kernel
    /// then delivers exactly one terminal CQE, and the existing op_state
    /// CAS resolves the cancel-vs-completion race correctly.
    ///
    /// For non-io use cases, prefer ``on_cancel(callable)`` and have the
    /// coroutine poll an ``event`` / atomic flag, or wake via ``channel`` /
    /// ``condition_variable``.
    ///
    /// The implementation is retained (and routes through
    /// ``runtime::schedule_handle`` so that vthread_stack / promise context is
    /// restored before resume) for callers who can prove their handle is
    /// **not** suspended on an io awaitable. Otherwise this is a footgun.
    /// @param handle Coroutine to resume on cancellation
    /// @return Registration handle
    [[deprecated(
        "Unsafe with handles suspended on io_awaitables (double-resume race "
        "with op_state CAS). Pass a cancel_token to the awaitable instead "
        "(see time::sleep_for(d, token) / cancellable_sleep_awaitable), or "
        "use on_cancel(callable) with an event/atomic flag.")]]
    [[nodiscard]] registration on_cancel_resume(std::coroutine_handle<> handle) const {
        return on_cancel([handle]() {
            if (handle && !handle.done()) {
                runtime::schedule_handle(handle);
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
