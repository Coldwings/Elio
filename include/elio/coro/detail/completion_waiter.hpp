#pragma once

#include <cassert>
#include <coroutine>
#include <mutex>
#include <utility>

namespace elio::coro::detail {

class completion_waiter_slot;

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
        assert(waiter_ == nullptr &&
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

    std::coroutine_handle<> take() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!waiter_) {
            return {};
        }

        auto handle = waiter_->handle_;
        waiter_->handle_ = {};
        waiter_ = nullptr;
        return handle;
    }

private:
    void remove(completion_waiter& waiter) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (waiter_ == &waiter) {
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

    friend class completion_waiter;
};

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
