#pragma once

#include "completion_waiter.hpp"

#include <coroutine>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace elio::runtime {
class scheduler;
void schedule_destruction_waiter(scheduler* owner,
                                 std::coroutine_handle<> handle) noexcept;
}

namespace elio::coro::detail {

class destruction_waiters final {
public:
    struct observation;
    using observation_list = std::list<std::shared_ptr<observation>>;

    struct observation final {
        explicit observation(runtime::scheduler* owner) noexcept
            : waiter(std::in_place, slot), scheduler(owner) {}

        completion_waiter_slot slot;
        std::optional<completion_waiter> waiter;
        runtime::scheduler* scheduler;
        observation_list::iterator position;
        bool linked = false;
    };

    template<typename Ready>
    bool register_waiter(std::shared_ptr<observation> observer,
                         std::coroutine_handle<> handle, Ready&& ready) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::forward<Ready>(ready)()) {
            return false;
        }
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (pause_registration_for_test.load(std::memory_order_acquire)) {
            registration_paused_for_test.store(true, std::memory_order_release);
            while (pause_registration_for_test.load(std::memory_order_acquire)) {
                pause_registration_for_test.wait(true, std::memory_order_acquire);
            }
        }
#endif
        observations_.push_back(observer);
        observer->position = std::prev(observations_.end());
        observer->linked = true;
        // Publication may now race with this registration, but its notifier
        // takes the same list lock and will see the fully registered slot.
        const bool registered = observer->slot.register_waiter(
            *observer->waiter, handle, [] { return false; });
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (registered) {
            registered_count_for_test.fetch_add(1, std::memory_order_release);
        }
#endif
        return registered;
    }

    void abandon(const std::shared_ptr<observation>& observer) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (observer->linked) {
                observations_.erase(observer->position);
                observer->linked = false;
            }
        }
        // The producer may already own this node. Removing the registration
        // also invalidates its selected-but-unclaimed wake lease.
        observer->waiter.reset();
    }

    void notify_destroyed() noexcept {
        observation_list pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(observations_);
            for (const auto& observer : pending) {
                observer->linked = false;
            }
        }
        for (const auto& observer : pending) {
            auto wake = observer->slot.take();
            auto handle = wake.claim();
            if (handle) {
                runtime::schedule_destruction_waiter(
                    observer->scheduler, handle);
            }
        }
    }

#ifdef ELIO_RUNTIME_TEST_HOOKS
    inline static std::atomic<bool> pause_registration_for_test{false};
    inline static std::atomic<bool> registration_paused_for_test{false};
    inline static std::atomic<size_t> registered_count_for_test{0};

    size_t pending_count_for_test() {
        std::lock_guard<std::mutex> lock(mutex_);
        return observations_.size();
    }
#endif

private:
    std::mutex mutex_;
    observation_list observations_;
};

} // namespace elio::coro::detail
