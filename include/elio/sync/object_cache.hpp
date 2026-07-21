#pragma once

#include <elio/coro/task.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/time/timer.hpp>
#include <elio/log/macros.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace elio::sync {

struct object_cache_config {
    size_t num_shards = 64;
    std::chrono::milliseconds reclaim_delay{30000};
    std::chrono::milliseconds sweep_interval{5000};
    std::chrono::milliseconds default_ttl{0};
};

namespace detail_oc {

inline size_t round_up_power_of_two(size_t v) noexcept {
    if (v == 0) return 1;
    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    if constexpr (sizeof(size_t) > 4) {
        v |= v >> 32;
    }
    return v + 1;
}

inline int64_t steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef ELIO_OBJECT_CACHE_TEST_HOOKS
struct release_waiter_published_test_hook {
    std::atomic<void (*)(void*)> callback{nullptr};
    std::atomic<void*> context{nullptr};

    void run() const noexcept {
        auto* cb = callback.load(std::memory_order_acquire);
        if (cb) {
            cb(context.load(std::memory_order_acquire));
        }
    }
};

inline release_waiter_published_test_hook release_waiter_published_hook;
inline std::atomic<size_t> release_waiter_probes_for_test{0};
#endif

} // namespace detail_oc

template<typename Key, typename Value,
         typename Hash = std::hash<Key>,
         typename KeyEqual = std::equal_to<Key>>
class object_cache {
public:
    class borrow;

private:
    struct entry {
        enum class state : uint8_t { constructing, ready, failed };

        std::atomic<state> state_{state::constructing};
        std::atomic<int64_t> refcount_{0};
        std::unique_ptr<Value> value_;

        event ready_event_;
        std::exception_ptr error_;

        std::atomic<bool> force_evict_{false};
        int64_t created_at_ns_{detail_oc::steady_now_ns()};
        std::chrono::milliseconds ttl_{0};

        std::atomic<bool> release_requested_{false};
        coro::detail::completion_waiter_slot release_waiter_;

        entry* reclaim_prev = nullptr;
        entry* reclaim_next = nullptr;
        bool in_reclaim_list = false;
        int64_t idle_since_ns_ = 0;

        Key key_;
        size_t shard_index_ = 0;
    };

    struct shard {
        mutable std::mutex mutex;
        std::unordered_map<Key, std::shared_ptr<entry>, Hash, KeyEqual> map;
        entry* reclaim_head = nullptr;
        entry* reclaim_tail = nullptr;

        void reclaim_push_back(entry* e) noexcept {
            e->in_reclaim_list = true;
            e->reclaim_prev = reclaim_tail;
            e->reclaim_next = nullptr;
            if (reclaim_tail) {
                reclaim_tail->reclaim_next = e;
            } else {
                reclaim_head = e;
            }
            reclaim_tail = e;
        }

        void reclaim_unlink(entry* e) noexcept {
            if (!e->in_reclaim_list) return;
            if (e->reclaim_prev) {
                e->reclaim_prev->reclaim_next = e->reclaim_next;
            } else {
                reclaim_head = e->reclaim_next;
            }
            if (e->reclaim_next) {
                e->reclaim_next->reclaim_prev = e->reclaim_prev;
            } else {
                reclaim_tail = e->reclaim_prev;
            }
            e->reclaim_prev = nullptr;
            e->reclaim_next = nullptr;
            e->in_reclaim_list = false;
        }
    };

    struct internals {
        std::unique_ptr<shard[]> shards_;
        size_t num_shards_;
        size_t shard_mask_;
        object_cache_config cfg_;
        std::atomic<bool> sweep_started_{false};

        explicit internals(object_cache_config cfg)
            : cfg_(std::move(cfg)) {
            num_shards_ = detail_oc::round_up_power_of_two(
                cfg_.num_shards > 0 ? cfg_.num_shards : 64);
            shard_mask_ = num_shards_ - 1;
            shards_ = std::make_unique<shard[]>(num_shards_);
        }

        shard& shard_for(const Key& key) noexcept {
            return shards_[Hash{}(key) & shard_mask_];
        }

        void sweep_expired() {
            auto now_ns = detail_oc::steady_now_ns();
            auto reclaim_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                cfg_.reclaim_delay).count();
            auto default_ttl_ns = cfg_.default_ttl.count() > 0
                ? std::chrono::duration_cast<std::chrono::nanoseconds>(cfg_.default_ttl).count()
                : int64_t{0};

            for (size_t si = 0; si < num_shards_; ++si) {
                auto& s = shards_[si];
                std::vector<std::shared_ptr<entry>> to_destroy;

                {
                    std::lock_guard lock(s.mutex);
                    auto* cur = s.reclaim_head;
                    while (cur) {
                        auto* next = cur->reclaim_next;

                        if (cur->refcount_.load(std::memory_order_acquire) > 0) {
                            s.reclaim_unlink(cur);
                            cur = next;
                            continue;
                        }

                        auto entry_ttl_ns = cur->ttl_.count() > 0
                            ? std::chrono::duration_cast<std::chrono::nanoseconds>(cur->ttl_).count()
                            : default_ttl_ns;

                        bool ttl_expired = entry_ttl_ns > 0 &&
                            (now_ns - cur->created_at_ns_ >= entry_ttl_ns);
                        bool reclaim_ready = (now_ns - cur->idle_since_ns_ >= reclaim_ns);

                        if (ttl_expired || reclaim_ready) {
                            auto it = s.map.find(cur->key_);
                            if (it != s.map.end() && it->second.get() == cur) {
                                to_destroy.push_back(std::move(it->second));
                                s.map.erase(it);
                            }
                            s.reclaim_unlink(cur);
                        }

                        cur = next;
                    }
                }

                // Sweep TTL-expired entries with active borrows (not in reclaim list)
                {
                    std::vector<std::shared_ptr<entry>> ttl_destroy;

                    {
                        std::lock_guard lock(s.mutex);
                        std::vector<Key> evict_keys;
                        for (auto& [k, e] : s.map) {
                            if (e->state_.load(std::memory_order_acquire)
                                != entry::state::ready) {
                                continue;
                            }
                            auto entry_ttl_ns2 = e->ttl_.count() > 0
                                ? std::chrono::duration_cast<std::chrono::nanoseconds>(e->ttl_).count()
                                : default_ttl_ns;
                            if (entry_ttl_ns2 > 0 &&
                                (now_ns - e->created_at_ns_ >= entry_ttl_ns2)) {
                                e->force_evict_.store(true, std::memory_order_release);
                                s.reclaim_unlink(e.get());
                                evict_keys.push_back(k);
                            }
                        }
                        for (auto& k : evict_keys) {
                            auto it = s.map.find(k);
                            if (it != s.map.end()) {
                                ttl_destroy.push_back(std::move(it->second));
                                s.map.erase(it);
                            }
                        }
                    }
                }
            }
        }

        void enqueue_for_reclaim(entry* e) {
            auto& s = shards_[e->shard_index_];
            std::lock_guard lock(s.mutex);
            auto it = s.map.find(e->key_);
            if (it == s.map.end() || it->second.get() != e) return;
            if (e->refcount_.load(std::memory_order_acquire) > 0) return;
            if (e->in_reclaim_list) return;
            if (e->force_evict_.load(std::memory_order_acquire)) return;
            e->idle_since_ns_ = detail_oc::steady_now_ns();
            s.reclaim_push_back(e);
        }

        void remove_from_index(entry* e) {
            auto& s = shards_[e->shard_index_];
            std::lock_guard lock(s.mutex);
            s.reclaim_unlink(e);
            auto it = s.map.find(e->key_);
            if (it != s.map.end() && it->second.get() == e) {
                s.map.erase(it);
            }
        }
    };

    class construction_guard {
    public:
        construction_guard(std::shared_ptr<internals> state,
                           shard& owner_shard,
                           std::shared_ptr<entry> constructing) noexcept
            : state_(std::move(state))
            , shard_(&owner_shard)
            , entry_(std::move(constructing)) {}

        ~construction_guard() {
            if (committed_ || !entry_) return;

            {
                std::lock_guard lock(shard_->mutex);
                shard_->reclaim_unlink(entry_.get());
                auto it = shard_->map.find(entry_->key_);
                if (it != shard_->map.end() && it->second.get() == entry_.get()) {
                    shard_->map.erase(it);
                }
            }

            entry_->state_.store(entry::state::failed, std::memory_order_release);
            entry_->ready_event_.set();
        }

        void commit() noexcept {
            committed_ = true;
        }

    private:
        std::shared_ptr<internals> state_;
        shard* shard_ = nullptr;
        std::shared_ptr<entry> entry_;
        bool committed_ = false;
    };

    class release_awaitable {
    public:
        explicit release_awaitable(std::shared_ptr<entry> e) noexcept
            : entry_sp_(std::move(e))
            , waiter_(entry_sp_->release_waiter_) {}

        bool await_ready() const noexcept {
            return entry_sp_->refcount_.load(std::memory_order_acquire) == 1;
        }

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            auto pin = entry_sp_;
            bool suspend = pin->release_waiter_.register_waiter(
                waiter_, h, [&] {
                    return pin->refcount_.load(std::memory_order_acquire) == 1;
                });
#ifdef ELIO_OBJECT_CACHE_TEST_HOOKS
            if (suspend) {
                detail_oc::release_waiter_published_hook.run();
            }
#endif
            return suspend;
        }

        void await_resume() const noexcept {}

    private:
        std::shared_ptr<entry> entry_sp_;
        coro::detail::completion_waiter waiter_;
    };

public:
    class borrow {
    public:
        borrow() = default;

        ~borrow() {
            reset();
        }

        borrow(borrow&& other) noexcept
            : entry_(std::move(other.entry_))
            , internals_(std::move(other.internals_)) {}

        borrow& operator=(borrow&& other) noexcept {
            if (this != &other) {
                reset();
                entry_ = std::move(other.entry_);
                internals_ = std::move(other.internals_);
            }
            return *this;
        }

        borrow(const borrow&) = delete;
        borrow& operator=(const borrow&) = delete;

        Value& operator*() { return *entry_->value_; }
        const Value& operator*() const { return *entry_->value_; }
        Value* operator->() { return entry_->value_.get(); }
        const Value* operator->() const { return entry_->value_.get(); }
        Value* get() noexcept { return entry_ ? entry_->value_.get() : nullptr; }
        const Value* get() const noexcept { return entry_ ? entry_->value_.get() : nullptr; }

        explicit operator bool() const noexcept {
            return entry_ && entry_->value_;
        }

        void mark_evict() {
            if (!entry_) return;
            bool already = entry_->force_evict_.exchange(true, std::memory_order_acq_rel);
            if (!already) {
                auto state = internals_.lock();
                if (state) {
                    state->remove_from_index(entry_.get());
                }
            }
        }

        coro::task<std::unique_ptr<Value>> release() {
            if (!entry_) co_return nullptr;

            bool already = entry_->release_requested_.exchange(
                true, std::memory_order_acq_rel);
            if (already) co_return nullptr;

            auto state = internals_.lock();
            if (state) {
                state->remove_from_index(entry_.get());
            }

            if (entry_->refcount_.load(std::memory_order_acquire) > 1) {
                co_await release_awaitable(entry_);
            }

            auto value = std::move(entry_->value_);
            entry_->refcount_.fetch_sub(1, std::memory_order_release);
            entry_.reset();
            co_return value;
        }

    private:
        friend class object_cache;

        borrow(std::shared_ptr<entry> e, std::weak_ptr<internals> state) noexcept
            : entry_(std::move(e))
            , internals_(std::move(state)) {}

        void reset() noexcept {
            if (!entry_) return;

            auto prev = entry_->refcount_.fetch_sub(1, std::memory_order_acq_rel);

            if (prev == 2) {
                // Always rendezvous with register_waiter(): checking a separate
                // release flag here permits both threads to observe stale state.
#ifdef ELIO_OBJECT_CACHE_TEST_HOOKS
                detail_oc::release_waiter_probes_for_test.fetch_add(
                    1, std::memory_order_relaxed);
#endif
                auto waiter = entry_->release_waiter_.take();
                if (waiter) {
                    runtime::schedule_handle(waiter);
                }
            } else if (prev == 1) {
                if (!entry_->force_evict_.load(std::memory_order_acquire)) {
                    auto state = internals_.lock();
                    if (state) {
                        state->enqueue_for_reclaim(entry_.get());
                    }
                }
            }

            entry_.reset();
        }

        std::shared_ptr<entry> entry_;
        std::weak_ptr<internals> internals_;
    };

    explicit object_cache(object_cache_config cfg = {})
        : state_(std::make_shared<internals>(std::move(cfg))) {}

    ~object_cache() {
        sweep_cancel_.cancel();
    }

    object_cache(const object_cache&) = delete;
    object_cache& operator=(const object_cache&) = delete;
    object_cache(object_cache&&) = delete;
    object_cache& operator=(object_cache&&) = delete;

    template<typename Ctor>
    coro::task<borrow> get(const Key& key, Ctor&& ctor) {
        co_return co_await get_impl(key, std::forward<Ctor>(ctor),
                                     std::chrono::milliseconds{0});
    }

    template<typename Ctor>
    coro::task<borrow> get(const Key& key, Ctor&& ctor,
                            std::chrono::milliseconds ttl) {
        co_return co_await get_impl(key, std::forward<Ctor>(ctor), ttl);
    }

    void evict(const Key& key) {
        std::shared_ptr<entry> evicted;
        {
            auto& s = state_->shard_for(key);
            std::lock_guard lock(s.mutex);
            auto it = s.map.find(key);
            if (it != s.map.end()) {
                auto& e = it->second;
                e->force_evict_.store(true, std::memory_order_release);
                s.reclaim_unlink(e.get());
                evicted = std::move(it->second);
                s.map.erase(it);
            }
        }
    }

    size_t size() const noexcept {
        size_t total = 0;
        for (size_t i = 0; i < state_->num_shards_; ++i) {
            auto& s = state_->shards_[i];
            std::lock_guard lock(s.mutex);
            total += s.map.size();
        }
        return total;
    }

    size_t active_count() const noexcept {
        size_t total = 0;
        for (size_t i = 0; i < state_->num_shards_; ++i) {
            auto& s = state_->shards_[i];
            std::lock_guard lock(s.mutex);
            for (auto& [k, e] : s.map) {
                if (e->refcount_.load(std::memory_order_relaxed) > 0) {
                    ++total;
                }
            }
        }
        return total;
    }

private:
    static coro::task<void> sweep_coroutine(std::weak_ptr<internals> weak,
                                              std::chrono::milliseconds interval,
                                              coro::cancel_token token) {
        while (!token.is_cancelled()) {
            co_await time::sleep_for(interval, token);
            if (token.is_cancelled()) break;
            auto state = weak.lock();
            if (!state) break;
            state->sweep_expired();
        }
    }

    void ensure_sweep_running() {
        if (state_->sweep_started_.load(std::memory_order_acquire)) return;
        auto* sched = runtime::get_current_scheduler();
        if (!sched) return;
        if (state_->sweep_started_.exchange(true, std::memory_order_acq_rel)) return;

        struct sweep_started_guard {
            internals* state;
            bool committed = false;

            ~sweep_started_guard() {
                if (!committed) {
                    state->sweep_started_.store(false, std::memory_order_release);
                }
            }
        } started_guard{state_.get()};

        auto t = sweep_coroutine(
            std::weak_ptr<internals>(state_), state_->cfg_.sweep_interval,
            sweep_cancel_.get_token());

        auto handle = coro::detail::task_access::release(std::move(t));
        handle.promise().detached_ = true;
        handle.promise().detach_from_parent();
        if (sched->try_spawn(handle)) {
            started_guard.committed = true;
        } else {
            handle.destroy();
        }
    }

    template<typename Ctor>
    coro::task<borrow> get_impl(const Key& key, Ctor&& ctor,
                                 std::chrono::milliseconds ttl) {
        ensure_sweep_running();

        auto& s = state_->shard_for(key);

        for (;;) {
            std::shared_ptr<entry> e;
            bool i_am_constructor = false;
            std::shared_ptr<entry> stale;

            {
                std::lock_guard lock(s.mutex);
                auto it = s.map.find(key);

                if (it != s.map.end()) {
                    e = it->second;
                    auto st = e->state_.load(std::memory_order_acquire);

                    if (st == entry::state::ready) {
                        if (!is_ttl_expired(e.get())) {
                            e->refcount_.fetch_add(1, std::memory_order_relaxed);
                            s.reclaim_unlink(e.get());
                            co_return borrow(std::move(e), state_);
                        }
                        e->force_evict_.store(true, std::memory_order_release);
                        s.reclaim_unlink(e.get());
                        stale = std::move(it->second);
                        s.map.erase(it);
                        e.reset();
                    } else if (st == entry::state::failed) {
                        stale = std::move(it->second);
                        s.map.erase(it);
                        e.reset();
                    }
                }

                if (!e) {
                    e = std::make_shared<entry>();
                    e->key_ = key;
                    e->shard_index_ = static_cast<size_t>(&s - state_->shards_.get());
                    e->ttl_ = ttl;
                    s.map[key] = e;
                    i_am_constructor = true;
                }
            }

            if (!i_am_constructor) {
                co_await e->ready_event_.wait();

                auto st = e->state_.load(std::memory_order_acquire);
                if (st == entry::state::ready) {
                    {
                        std::lock_guard lock(s.mutex);
                        auto it = s.map.find(key);
                        if (it == s.map.end() || it->second.get() != e.get()) {
                            continue;
                        }
                        e->refcount_.fetch_add(1, std::memory_order_relaxed);
                        s.reclaim_unlink(e.get());
                    }
                    co_return borrow(std::move(e), state_);
                }
                if (e->error_) {
                    std::rethrow_exception(e->error_);
                }
                throw std::runtime_error("object_cache: construction failed");
            }

            construction_guard guard(state_, s, e);

            try {
                auto value = co_await std::forward<Ctor>(ctor)();
                e->value_ = std::make_unique<Value>(std::move(value));
                e->refcount_.fetch_add(1, std::memory_order_relaxed);
                e->state_.store(entry::state::ready, std::memory_order_release);
                e->ready_event_.set();
                guard.commit();
                co_return borrow(std::move(e), state_);
            } catch (...) {
                e->error_ = std::current_exception();
                e->state_.store(entry::state::failed, std::memory_order_release);
                e->ready_event_.set();
                {
                    std::lock_guard lock(s.mutex);
                    auto it = s.map.find(key);
                    if (it != s.map.end() && it->second == e) {
                        s.map.erase(it);
                    }
                }
                guard.commit();
                throw;
            }
        }
    }

    bool is_ttl_expired(const entry* e) const noexcept {
        auto entry_ttl = e->ttl_.count() > 0 ? e->ttl_ : state_->cfg_.default_ttl;
        if (entry_ttl.count() <= 0) return false;
        auto ttl_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(entry_ttl).count();
        return detail_oc::steady_now_ns() - e->created_at_ns_ >= ttl_ns;
    }

    coro::cancel_source sweep_cancel_;
    std::shared_ptr<internals> state_;
};

} // namespace elio::sync
