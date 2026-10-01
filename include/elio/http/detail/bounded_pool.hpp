#pragma once

#include <elio/http/pool_limits.hpp>
#include <elio/http/client_result.hpp>
#include <elio/http/detail/route_plan.hpp>
#include <elio/coro/join_wait.hpp>
#include <elio/detail/intrusive_list.hpp>

#include <cassert>
#include <chrono>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace elio::http::detail {

#ifdef ELIO_RUNTIME_TEST_HOOKS
inline std::atomic<void(*)()> pool_waiter_queued_for_test{nullptr};
inline std::atomic<void(*)()> pool_waiter_notified_for_test{nullptr};
#endif

template<typename Stream>
class bounded_pool {
    struct state;
    struct bucket;
    struct waiter;

public:
    class permit {
    public:
        permit() noexcept = default;
        permit(const permit&) = delete;
        permit& operator=(const permit&) = delete;
        permit(permit&& other) noexcept
            : owner_(std::move(other.owner_)), route_(std::move(other.route_)),
              dialing_(std::exchange(other.dialing_, false)) {}
        permit& operator=(permit&& other) noexcept {
            if (this != &other) {
                reset();
                owner_ = std::move(other.owner_);
                route_ = std::move(other.route_);
                dialing_ = std::exchange(other.dialing_, false);
            }
            return *this;
        }
        ~permit() { reset(); }
        void reset() noexcept;
        void dial_complete() noexcept;

    private:
        friend class bounded_pool;
        friend struct state;
        permit(std::shared_ptr<state> owner, std::shared_ptr<bucket> route,
               bool dialing) noexcept
            : owner_(std::move(owner)), route_(std::move(route)), dialing_(dialing) {}
        std::shared_ptr<state> owner_;
        std::shared_ptr<bucket> route_;
        bool dialing_ = false;
    };

    struct grant {
        permit capacity;
        std::optional<Stream> idle;
    };

private:
    struct idle_entry {
        explicit idle_entry(Stream value) : stream(std::move(value)) {}
        // Detached idle entries acquire their retiring permit before unlinking.
        // Member order closes the stream before releasing physical capacity.
        permit capacity;
        Stream stream;
    };

public:

    // Both stream destruction and notification must occur outside the caller's
    // lifecycle lock, not merely outside the admission mutex.
    struct change {
        change() = default;
        change(const change&) = delete;
        change& operator=(const change&) = delete;
        change(change&&) noexcept = default;
        change& operator=(change&&) = delete;
        ~change() {
            retired.clear();
            notify();
        }
        void notify() noexcept {
            while (notifications) {
                auto node = std::move(notifications);
                notifications = std::move(node->next_notification);
                node->ready->set_value();
#ifdef ELIO_RUNTIME_TEST_HOOKS
                if (auto hook = pool_waiter_notified_for_test.load(std::memory_order_acquire)) hook();
#endif
            }
        }
        bool retained = false;
        std::list<idle_entry> retired;
        std::shared_ptr<waiter> notifications;
    };

    explicit bounded_pool(pool_limits limits, std::chrono::seconds idle_timeout)
        : owner_(std::make_shared<state>(limits, idle_timeout)) {}

    coro::task<client_result<grant>> acquire(connection_key key,
            std::optional<std::chrono::steady_clock::time_point> deadline,
            coro::cancel_token token) {
        return acquire_owned(owner_, std::move(key), deadline, std::move(token));
    }

    change retain(permit& capacity, Stream& stream) {
        change result;
        auto owner = capacity.owner_;
        if (!owner) return result;
        {
            std::lock_guard lock(owner->mutex);
            auto& route = *capacity.route_;
            if (owner->closing || capacity.dialing_ ||
                route.idle.size() >= owner->limits.max_idle_per_route ||
                owner->idle >= owner->limits.max_idle_total) return result;
            // Allocation failure leaves the lease and its permit unchanged.
            route.idle.emplace_back(std::move(stream));
            route.idle.back().stream.touch();
            ++owner->idle;
            capacity.owner_.reset();
            capacity.route_.reset();
            result.retained = true;
            owner->dispatch(result);
        }
        return result;
    }

    change clear(bool close = false) {
        change result;
        {
            std::lock_guard lock(owner_->mutex);
            owner_->closing = owner_->closing || close;
            for (auto& [key, route] : owner_->routes) {
                (void)key;
                for (auto& entry : route->idle)
                    entry.capacity = permit(owner_, route, false);
                result.retired.splice(result.retired.end(), route->idle);
            }
            owner_->idle = 0;
            owner_->prune_empty();
            if (owner_->closing) {
                while (auto* node = owner_->waiters.pop_front()) {
                    --node->route->waiting;
                    node->error = ESHUTDOWN;
                    owner_->notify(*node, result);
                }
                owner_->prune_empty();
            } else owner_->dispatch(result);
        }
        return result;
    }

#ifdef ELIO_RUNTIME_TEST_HOOKS
    struct counters {
        size_t live, idle, dialing, waiting, routes;
    };
    counters counters_for_test() const {
        std::lock_guard lock(owner_->mutex);
        return {owner_->live, owner_->idle, owner_->dialing,
                owner_->waiters.size(), owner_->routes.size()};
    }
#endif

private:
    struct bucket {
        explicit bucket(connection_key value) : key(std::move(value)) {}
        connection_key key;
        size_t live = 0;
        size_t waiting = 0;
        std::list<idle_entry> idle;
    };

    struct waiter : elio::detail::intrusive_list_node<waiter>,
                    std::enable_shared_from_this<waiter> {
        std::shared_ptr<bucket> route;
        std::shared_ptr<coro::detail::join_state<void>> ready =
            std::make_shared<coro::detail::join_state<void>>();
        std::optional<grant> selected;
        int error = 0;
        std::shared_ptr<waiter> next_notification;
    };

    struct state : std::enable_shared_from_this<state> {
        state(pool_limits value, std::chrono::seconds timeout)
            : limits(value), idle_timeout(timeout) {}

        void prune_empty() noexcept {
            for (auto it = routes.begin(); it != routes.end();) {
                if (it->second->live == 0 && it->second->waiting == 0)
                    it = routes.erase(it);
                else ++it;
            }
        }

        void expire(change& result) noexcept {
            const auto now = std::chrono::steady_clock::now();
            for (auto& [key, route] : routes) {
                (void)key;
                while (!route->idle.empty() &&
                       now - route->idle.front().stream.last_use() >= idle_timeout) {
                    route->idle.front().capacity = permit(this->shared_from_this(), route, false);
                    --idle;
                    result.retired.splice(result.retired.end(), route->idle,
                                          route->idle.begin());
                }
            }
            prune_empty();
        }

        bool available(const bucket& route, change& result) noexcept {
            if (!route.idle.empty()) return true;
            if (route.live >= limits.max_live_per_route || dialing >= limits.max_dials_total)
                return false;
            if (live >= limits.max_live_total) {
                for (auto& [key, candidate] : routes) {
                    (void)key;
                    if (!candidate->idle.empty()) {
                        candidate->idle.front().capacity =
                            permit(this->shared_from_this(), candidate, false);
                        --idle;
                        result.retired.splice(result.retired.end(), candidate->idle,
                                              candidate->idle.begin());
                        break;
                    }
                }
            }
            return live < limits.max_live_total;
        }

        grant reserve(const std::shared_ptr<bucket>& route) noexcept {
            const bool needs_dial = route->idle.empty();
            grant result{permit(this->shared_from_this(), route, needs_dial), {}};
            if (needs_dial) {
                ++route->live;
                ++live;
                ++dialing;
            } else {
                result.idle.emplace(std::move(route->idle.front().stream));
                route->idle.pop_front();
                --idle;
                result.idle->touch();
            }
            return result;
        }

        void notify(waiter& node, change& result) noexcept {
            auto current = node.shared_from_this();
            current->next_notification = std::move(result.notifications);
            result.notifications = std::move(current);
        }

        void dispatch(change& result) noexcept {
            // Global FIFO intentionally does not let new acquisitions bypass a
            // saturated head route. No polling or scheduler-worker blocking.
            expire(result);
            while (auto* node = waiters.front()) {
                if (!available(*node->route, result)) break;
                waiters.pop_front();
                --node->route->waiting;
                node->selected.emplace(reserve(node->route));
                notify(*node, result);
            }
            prune_empty();
        }

        const pool_limits limits;
        const std::chrono::seconds idle_timeout;
        std::mutex mutex;
        std::unordered_map<connection_key, std::shared_ptr<bucket>, connection_key_hash> routes;
        elio::detail::intrusive_list<waiter> waiters;
        size_t live = 0, idle = 0, dialing = 0;
        bool closing = false;
    };

    class departure {
    public:
        departure(std::shared_ptr<state> owner, std::shared_ptr<waiter> node)
            : owner_(std::move(owner)), node_(std::move(node)) {}
        ~departure() {
            change result;
            std::optional<grant> recovered;
            {
                std::lock_guard lock(owner_->mutex);
                if (node_->is_linked()) {
                    owner_->waiters.remove(node_.get());
                    --node_->route->waiting;
                }
                recovered = std::move(node_->selected);
                owner_->dispatch(result);
            }
            // Close an unused selected stream before releasing its live permit.
            if (recovered) {
                recovered->idle.reset();
                recovered->capacity.reset();
            }
        }
    private:
        std::shared_ptr<state> owner_;
        std::shared_ptr<waiter> node_;
    };

    static coro::task<client_result<grant>> acquire_owned(std::shared_ptr<state> owner,
            connection_key key,
            std::optional<std::chrono::steady_clock::time_point> deadline,
            coro::cancel_token token) {
        if (token.is_cancelled())
            co_return make_client_error(ECANCELED, client_stage::acquire);
        if (deadline && *deadline <= std::chrono::steady_clock::now())
            co_return make_client_error(ETIMEDOUT, client_stage::acquire);
        auto node = std::make_shared<waiter>();
        departure cleanup(owner, node);
        for (;;) {
            change expired;
            std::lock_guard lock(owner->mutex);
            if (owner->closing)
                co_return make_client_error(ESHUTDOWN, client_stage::acquire);
            owner->expire(expired);
            auto it = owner->routes.find(key);
            if (it == owner->routes.end()) {
                if (owner->limits.max_route_buckets == 0)
                    co_return make_client_error(EAGAIN, client_stage::acquire);
                // At metadata capacity, evict one idle-only bucket rather than
                // retaining resources that prevent a new route from admission.
                if (owner->routes.size() >= owner->limits.max_route_buckets) {
                    for (auto candidate = owner->routes.begin(); candidate != owner->routes.end();
                         ++candidate) {
                        auto& route = *candidate->second;
                        if (route.waiting == 0 && route.live == route.idle.size()) {
                            owner->idle -= route.idle.size();
                            for (auto& entry : route.idle)
                                entry.capacity = permit(owner, candidate->second, false);
                            expired.retired.splice(expired.retired.end(), route.idle);
                            break;
                        }
                    }
                    if (!expired.retired.empty()) continue;
                    if (owner->routes.size() >= owner->limits.max_route_buckets)
                        co_return make_client_error(EAGAIN, client_stage::acquire);
                }
                it = owner->routes.emplace(key, std::make_shared<bucket>(key)).first;
            }
            node->route = it->second;
            if (owner->waiters.empty() && owner->available(*node->route, expired)) {
                co_return owner->reserve(node->route);
            }
            if (!expired.retired.empty() && owner->waiters.empty()) continue;
            if (owner->limits.max_live_per_route == 0 || owner->limits.max_live_total == 0 ||
                (owner->limits.max_dials_total == 0 && node->route->idle.empty()) ||
                owner->waiters.size() >= owner->limits.max_waiters_total) {
                owner->prune_empty();
                co_return make_client_error(EAGAIN, client_stage::acquire);
            }
            if (!runtime::scheduler::current()) {
                owner->prune_empty();
                co_return make_client_error(ENOTSUP, client_stage::acquire);
            }
            ++node->route->waiting;
            owner->waiters.push_back(node.get());
            break;
        }
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = pool_waiter_queued_for_test.load(std::memory_order_acquire)) hook();
#endif
        const auto outcome = co_await coro::detail::observe_join_result(node->ready, deadline, token);
        if (outcome == coro::join_wait_outcome::cancelled)
            co_return make_client_error(ECANCELED, client_stage::acquire);
        if (outcome == coro::join_wait_outcome::timed_out)
            co_return make_client_error(ETIMEDOUT, client_stage::acquire);
        {
            std::lock_guard lock(owner->mutex);
            if (node->error) co_return make_client_error(node->error, client_stage::acquire);
            assert(node->selected);
            co_return std::move(*node->selected);
        }
    }

    std::shared_ptr<state> owner_;
};

template<typename Stream>
void bounded_pool<Stream>::permit::reset() noexcept {
    auto owner = std::move(owner_);
    if (!owner) return;
    change result;
    {
        std::lock_guard lock(owner->mutex);
        assert(route_->live > 0 && owner->live > 0);
        assert(!dialing_ || owner->dialing > 0);
        if (dialing_) --owner->dialing;
        --route_->live;
        --owner->live;
        dialing_ = false;
        route_.reset();
        owner->dispatch(result);
    }
}

template<typename Stream>
void bounded_pool<Stream>::permit::dial_complete() noexcept {
    if (!owner_ || !dialing_) return;
    change result;
    {
        std::lock_guard lock(owner_->mutex);
        assert(owner_->dialing > 0);
        --owner_->dialing;
        dialing_ = false;
        owner_->dispatch(result);
    }
}

} // namespace elio::http::detail
