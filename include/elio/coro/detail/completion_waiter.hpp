#pragma once

#include <cassert>
#include <atomic>
#include <cstddef>
#include <coroutine>
#include <mutex>
#include <utility>

namespace elio::coro::detail {

class completion_waiter_slot;

#ifdef ELIO_RUNTIME_TEST_HOOKS
inline std::atomic<bool> pause_before_completion_wake_claim_for_test{false};
inline std::atomic<bool> completion_wake_claim_paused_for_test{false};
#endif

/// Move-only ownership of a completion wake selected by a producer.
///
/// The slot, rather than the awaiting coroutine frame, retains the selected
/// handle until claim() transfers scheduling ownership. This lets waiter
/// destruction abandon a wake after dequeue without leaving the producer with
/// a stale raw coroutine handle. The slot owner must outlive every unclaimed
/// lease returned by that slot.
class completion_wake_lease {
public:
    completion_wake_lease() noexcept = default;
    ~completion_wake_lease();

    completion_wake_lease(const completion_wake_lease&) = delete;
    completion_wake_lease& operator=(const completion_wake_lease&) = delete;

    completion_wake_lease(completion_wake_lease&& other) noexcept;
    completion_wake_lease& operator=(completion_wake_lease&& other) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return slot_ != nullptr;
    }

    /// Claim scheduling ownership of the selected handle. Returns an empty
    /// handle if waiter destruction abandoned this generation first.
    [[nodiscard]] std::coroutine_handle<> claim() noexcept;

private:
    completion_wake_lease(completion_waiter_slot& slot,
                          size_t generation) noexcept
        : slot_(&slot), generation_(generation) {}

    void reset() noexcept;

    completion_waiter_slot* slot_ = nullptr;
    size_t generation_ = 0;

    friend class completion_waiter_slot;
};

/// Awaiter-owned registration for a single completion waiter.
class completion_waiter {
public:
    completion_waiter() noexcept = default;

    explicit completion_waiter(completion_waiter_slot& slot) noexcept
        : slot_(&slot) {}

    explicit completion_waiter(completion_waiter_slot* slot) noexcept
        : slot_(slot) {}

    ~completion_waiter();

    completion_waiter(const completion_waiter&) = delete;
    completion_waiter& operator=(const completion_waiter&) = delete;

    completion_waiter(completion_waiter&& other) noexcept;
    completion_waiter& operator=(completion_waiter&& other) noexcept;

private:
    completion_waiter_slot* slot_ = nullptr;
    std::coroutine_handle<> handle_{};

    friend class completion_waiter_slot;
};

/// Thread-safe slot that never owns the registered awaiter node.
class completion_waiter_slot {
public:
    completion_waiter_slot() = default;

    ~completion_waiter_slot() {
        std::lock_guard<std::mutex> lock(mutex_);
        assert(waiter_ == nullptr && !selected() &&
               "completion waiter slot destroyed with a pending waiter");
    }

    completion_waiter_slot(const completion_waiter_slot&) = delete;
    completion_waiter_slot& operator=(const completion_waiter_slot&) = delete;

    template<typename Ready>
    bool register_waiter(completion_waiter& waiter,
                         std::coroutine_handle<> handle,
                         Ready&& ready) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        assert(waiter.slot_ == this);

        if (std::forward<Ready>(ready)()) {
            return false;
        }
        if (waiter_ != nullptr) {
            return false;
        }

        waiter.handle_ = handle;
        waiter_ = &waiter;
        return true;
    }

    completion_wake_lease take() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!waiter_ || selected()) {
            return {};
        }

        selected_handle_ = waiter_->handle_;
        ++generation_;
        waiter_->handle_ = {};
        return completion_wake_lease(*this, generation_);
    }

private:
    [[nodiscard]] bool selected() const noexcept {
        return (generation_ & size_t{1}) != 0;
    }

    std::coroutine_handle<> claim(size_t generation) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!selected() || generation_ != generation) {
            return {};
        }

        auto handle = selected_handle_;
        ++generation_;
        waiter_ = nullptr;
        selected_handle_ = {};
        return handle;
    }

    void abandon(size_t generation) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!selected() || generation_ != generation) {
            return;
        }
        ++generation_;
        waiter_ = nullptr;
        selected_handle_ = {};
    }

    void remove(completion_waiter& waiter) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (waiter_ == &waiter) {
            if (selected()) {
                ++generation_;
                selected_handle_ = {};
            }
            waiter_ = nullptr;
        }
        waiter.handle_ = {};
    }

    void move(completion_waiter& from, completion_waiter& to) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        assert(to.slot_ == nullptr);

        to.slot_ = this;
        to.handle_ = from.handle_;
        if (waiter_ == &from) {
            waiter_ = &to;
        }
        from.slot_ = nullptr;
        from.handle_ = {};
    }

    std::mutex mutex_;
    completion_waiter* waiter_ = nullptr;
    std::coroutine_handle<> selected_handle_{};
    // Odd generations hold a selected, cancelable wake. Claim or abandonment
    // advances to the next even generation before the slot can be reused.
    size_t generation_ = 0;

    friend class completion_waiter;
    friend class completion_wake_lease;
};

inline completion_wake_lease::~completion_wake_lease() {
    reset();
}

inline completion_wake_lease::completion_wake_lease(
    completion_wake_lease&& other) noexcept
    : slot_(std::exchange(other.slot_, nullptr))
    , generation_(std::exchange(other.generation_, 0)) {}

inline completion_wake_lease& completion_wake_lease::operator=(
    completion_wake_lease&& other) noexcept {
    if (this != &other) {
        reset();
        slot_ = std::exchange(other.slot_, nullptr);
        generation_ = std::exchange(other.generation_, 0);
    }
    return *this;
}

inline std::coroutine_handle<> completion_wake_lease::claim() noexcept {
#ifdef ELIO_RUNTIME_TEST_HOOKS
    if (pause_before_completion_wake_claim_for_test.load(
            std::memory_order_acquire)) {
        completion_wake_claim_paused_for_test.store(
            true, std::memory_order_release);
        completion_wake_claim_paused_for_test.notify_all();
        while (pause_before_completion_wake_claim_for_test.load(
            std::memory_order_acquire)) {
            pause_before_completion_wake_claim_for_test.wait(
                true, std::memory_order_acquire);
        }
        completion_wake_claim_paused_for_test.store(
            false, std::memory_order_release);
        completion_wake_claim_paused_for_test.notify_all();
    }
#endif
    auto* slot = std::exchange(slot_, nullptr);
    const auto generation = std::exchange(generation_, 0);
    return slot ? slot->claim(generation) : std::coroutine_handle<>{};
}

inline void completion_wake_lease::reset() noexcept {
    auto* slot = std::exchange(slot_, nullptr);
    const auto generation = std::exchange(generation_, 0);
    if (slot) {
        slot->abandon(generation);
    }
}

inline completion_waiter::~completion_waiter() {
    if (slot_) {
        slot_->remove(*this);
    }
}

inline completion_waiter::completion_waiter(
    completion_waiter&& other) noexcept {
    if (other.slot_) {
        other.slot_->move(other, *this);
    }
}

inline completion_waiter& completion_waiter::operator=(
    completion_waiter&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (slot_) {
        slot_->remove(*this);
        slot_ = nullptr;
    }
    if (other.slot_) {
        other.slot_->move(other, *this);
    }
    return *this;
}

} // namespace elio::coro::detail
