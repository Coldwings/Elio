#include <catch2/catch_test_macros.hpp>
#define ELIO_OBJECT_CACHE_TEST_HOOKS 1
#include <elio/sync/object_cache.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>
#include "../test_main.cpp"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace elio::sync;
using namespace elio::coro;
using namespace elio::runtime;

template<typename F>
auto spawn_joinable(scheduler& sched, F&& f) {
    return sched.go_joinable(std::forward<F>(f));
}

struct TrackedValue {
    int id;
    static std::atomic<int> ctor_count;
    static std::atomic<int> dtor_count;

    explicit TrackedValue(int i) : id(i) {
        ctor_count.fetch_add(1, std::memory_order_relaxed);
    }
    ~TrackedValue() {
        dtor_count.fetch_add(1, std::memory_order_relaxed);
    }

    TrackedValue(TrackedValue&& o) noexcept : id(o.id) { o.id = -1; }
    TrackedValue& operator=(TrackedValue&& o) noexcept {
        id = o.id; o.id = -1; return *this;
    }
    TrackedValue(const TrackedValue&) = delete;
    TrackedValue& operator=(const TrackedValue&) = delete;

    static void reset_counts() {
        ctor_count.store(0, std::memory_order_relaxed);
        dtor_count.store(0, std::memory_order_relaxed);
    }
};

std::atomic<int> TrackedValue::ctor_count{0};
std::atomic<int> TrackedValue::dtor_count{0};

class temporary_cache_key {
public:
    temporary_cache_key()
        : value_(std::make_unique<std::string>()) {}

    explicit temporary_cache_key(
        std::string value,
        std::shared_ptr<int> lifetime = {})
        : value_(std::make_unique<std::string>(std::move(value)))
        , lifetime_(std::move(lifetime)) {}

    temporary_cache_key(const temporary_cache_key& other)
        : value_(std::make_unique<std::string>(other.value()))
        , lifetime_(other.lifetime_) {}

    temporary_cache_key& operator=(const temporary_cache_key& other) {
        value_ = std::make_unique<std::string>(other.value());
        lifetime_ = other.lifetime_;
        return *this;
    }

    temporary_cache_key(temporary_cache_key&&) noexcept = default;
    temporary_cache_key& operator=(temporary_cache_key&&) noexcept = default;

    const std::string& value() const { return *value_; }

private:
    std::unique_ptr<std::string> value_;
    std::shared_ptr<int> lifetime_;
};

struct temporary_cache_key_hash {
    size_t operator()(const temporary_cache_key& key) const {
        return std::hash<std::string>{}(key.value());
    }
};

struct temporary_cache_key_equal {
    bool operator()(const temporary_cache_key& lhs,
                    const temporary_cache_key& rhs) const {
        return lhs.value() == rhs.value();
    }
};

class temporary_move_only_factory {
public:
    explicit temporary_move_only_factory(
        int value,
        std::shared_ptr<int> lifetime = {})
        : value_(std::make_unique<int>(value))
        , lifetime_(std::move(lifetime)) {}

    temporary_move_only_factory(temporary_move_only_factory&&) noexcept = default;
    temporary_move_only_factory& operator=(temporary_move_only_factory&&) noexcept = default;
    temporary_move_only_factory(const temporary_move_only_factory&) = delete;
    temporary_move_only_factory& operator=(const temporary_move_only_factory&) = delete;

    task<int> operator()() & = delete;
    task<int> operator()() const& = delete;
    task<int> operator()() && { return make_value(*value_); }

private:
    static task<int> make_value(int value) { co_return value; }

    std::unique_ptr<int> value_;
    std::shared_ptr<int> lifetime_;
};

class copy_only_cache_key {
public:
    copy_only_cache_key() = default;
    explicit copy_only_cache_key(std::string value) : value_(std::move(value)) {}

    copy_only_cache_key(const copy_only_cache_key&) = default;
    copy_only_cache_key& operator=(const copy_only_cache_key&) = default;
    copy_only_cache_key(copy_only_cache_key&&) = delete;
    copy_only_cache_key& operator=(copy_only_cache_key&&) = delete;

    const std::string& value() const noexcept { return value_; }

private:
    std::string value_;
};

struct copy_only_cache_key_hash {
    size_t operator()(const copy_only_cache_key& key) const {
        return std::hash<std::string>{}(key.value());
    }
};

struct copy_only_cache_key_equal {
    bool operator()(const copy_only_cache_key& lhs,
                    const copy_only_cache_key& rhs) const {
        return lhs.value() == rhs.value();
    }
};

class copy_only_factory {
public:
    explicit copy_only_factory(int value) : value_(value) {}

    copy_only_factory(const copy_only_factory&) = default;
    copy_only_factory& operator=(const copy_only_factory&) = default;
    copy_only_factory(copy_only_factory&&) = delete;
    copy_only_factory& operator=(copy_only_factory&&) = delete;

    task<int> operator()() & { co_return value_; }

private:
    int value_;
};

struct cvref_factory {
    int* observed = nullptr;

    task<int> operator()() & {
        *observed = 1;
        return make_value(11);
    }

    task<int> operator()() const& {
        *observed = 2;
        return make_value(22);
    }

    task<int> operator()() && {
        *observed = 3;
        return make_value(33);
    }

    task<int> operator()() const&& {
        *observed = 4;
        return make_value(44);
    }

private:
    static task<int> make_value(int value) { co_return value; }
};

struct reference_counting_factory {
    int* calls = nullptr;

    task<int> operator()() {
        ++*calls;
        co_return 91;
    }
};

TEST_CASE("object_cache owns stored get arguments",
          "[object_cache][lifetime][regression]") {
    using key_cache_type = object_cache<
        temporary_cache_key, int,
        temporary_cache_key_hash, temporary_cache_key_equal>;

    std::weak_ptr<int> key_lifetime;
    std::weak_ptr<int> factory_lifetime;

    {
        scheduler sched(1);
        {
            key_cache_type key_cache;

            auto key_token = std::make_shared<int>(1);
            auto factory_token = std::make_shared<int>(2);
            key_lifetime = key_token;
            factory_lifetime = factory_token;

            auto pending = key_cache.get(
                temporary_cache_key("temporary-key", std::move(key_token)),
                temporary_move_only_factory(73, std::move(factory_token)));

            const bool key_owned = !key_lifetime.expired();
            const bool factory_owned = !factory_lifetime.expired();
            CAPTURE(key_owned, factory_owned);
            REQUIRE(key_owned);
            REQUIRE(factory_owned);

            sched.start();
            auto h = sched.go_joinable(std::move(pending));

            h.wait_destroyed();
            auto borrowed = h.await_resume();
            REQUIRE(*borrowed == 73);
            REQUIRE(key_cache.size() == 1);
            REQUIRE(factory_lifetime.expired());
        }
        REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
    }

    REQUIRE(key_lifetime.expired());
    REQUIRE(factory_lifetime.expired());
}

TEST_CASE("object_cache preserves get constructor invocation category",
          "[object_cache][lifetime]") {
    scheduler sched(1);
    sched.start();

    std::array<int, 4> observed{};
    std::array<int, 4> values{};
    int ref_calls = 0;
    int ref_value = 0;

    {
        object_cache<std::string, int> cache;

        auto h = spawn_joinable(sched, [&]() -> task<void> {
            cvref_factory lvalue_factory{&observed[0]};
            auto lvalue = co_await cache.get("lvalue", lvalue_factory);
            values[0] = *lvalue;

            const cvref_factory const_lvalue_factory{&observed[1]};
            auto const_lvalue = co_await cache.get(
                "const-lvalue", const_lvalue_factory);
            values[1] = *const_lvalue;

            auto rvalue = co_await cache.get(
                "rvalue", cvref_factory{&observed[2]});
            values[2] = *rvalue;

            const cvref_factory const_rvalue_factory{&observed[3]};
            auto const_rvalue = co_await cache.get(
                "const-rvalue", std::move(const_rvalue_factory));
            values[3] = *const_rvalue;

            reference_counting_factory ref_factory{&ref_calls};
            auto ref_borrow = co_await cache.get("ref", std::ref(ref_factory));
            ref_value = *ref_borrow;
        });

        h.wait_destroyed();
        REQUIRE_NOTHROW(h.await_resume());
    }

    REQUIRE(observed[0] == 1);
    REQUIRE(observed[1] == 2);
    REQUIRE(observed[2] == 3);
    REQUIRE(observed[3] == 4);
    REQUIRE(values[0] == 11);
    REQUIRE(values[1] == 22);
    REQUIRE(values[2] == 33);
    REQUIRE(values[3] == 44);
    REQUIRE(ref_calls == 1);
    REQUIRE(ref_value == 91);
    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("object_cache accepts copy-only get lvalues",
          "[object_cache][lifetime]") {
    using cache_type = object_cache<
        copy_only_cache_key, int,
        copy_only_cache_key_hash, copy_only_cache_key_equal>;

    scheduler sched(1);
    sched.start();

    copy_only_cache_key key("copy-only");
    copy_only_factory factory(58);
    int result = 0;

    {
        cache_type cache;

        auto h = spawn_joinable(sched, [&]() -> task<void> {
            auto borrowed = co_await cache.get(key, factory);
            result = *borrowed;
        });

        h.wait_destroyed();
        REQUIRE_NOTHROW(h.await_resume());
        REQUIRE(result == 58);
        REQUIRE(cache.size() == 1);
    }

    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("object_cache basic get-or-create", "[object_cache]") {
    scheduler sched(1);
    sched.start();

    {
        object_cache<std::string, int> cache;

        auto h = spawn_joinable(sched, [&]() -> task<void> {
            auto b1 = co_await cache.get("key1", []() -> task<int> {
                co_return 42;
            });
            REQUIRE(*b1 == 42);

            auto b2 = co_await cache.get("key1", []() -> task<int> {
                co_return 999;
            });
            REQUIRE(*b2 == 42);

            auto b3 = co_await cache.get("key2", []() -> task<int> {
                co_return 100;
            });
            REQUIRE(*b3 == 100);

            REQUIRE(cache.size() == 2);
            co_return;
        });

        h.wait_destroyed();
    }

    sched.shutdown();
}

TEST_CASE("object_cache concurrent get deduplicates construction", "[object_cache]") {
    scheduler sched(4);
    sched.start();

    std::atomic<int> ctor_calls{0};
    std::atomic<int> completed{0};

    {
        object_cache<std::string, int> cache;

        constexpr int N = 20;
        std::vector<join_handle<void>> handles;

        auto* cache_ptr = &cache;
        auto* ctor_calls_ptr = &ctor_calls;
        auto* completed_ptr = &completed;

        for (int i = 0; i < N; ++i) {
            handles.push_back(spawn_joinable(sched, [=]() -> task<void> {
                auto b = co_await cache_ptr->get("shared_key", [=]() -> task<int> {
                    ctor_calls_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_await elio::time::sleep_for(std::chrono::milliseconds(10));
                    co_return 42;
                });
                REQUIRE(*b == 42);
                completed_ptr->fetch_add(1, std::memory_order_relaxed);
                co_return;
            }));
        }

        for (auto& h : handles) h.wait_destroyed();
    }

    REQUIRE(ctor_calls.load() == 1);
    REQUIRE(completed.load() == 20);
    sched.shutdown();
}

TEST_CASE("object_cache construction failure and retry", "[object_cache]") {
    scheduler sched(1);
    sched.start();

    std::atomic<int> attempt{0};

    {
        object_cache<std::string, int> cache;

        auto h = spawn_joinable(sched, [&]() -> task<void> {
            auto* cache_ptr = &cache;
            auto* attempt_ptr = &attempt;

            bool caught = false;
            try {
                co_await cache_ptr->get("fail_key", [=]() -> task<int> {
                    attempt_ptr->fetch_add(1, std::memory_order_relaxed);
                    throw std::runtime_error("construction failed");
                    co_return 0;
                });
            } catch (const std::runtime_error& e) {
                caught = true;
            }
            REQUIRE(caught);

            auto b = co_await cache_ptr->get("fail_key", [=]() -> task<int> {
                attempt_ptr->fetch_add(1, std::memory_order_relaxed);
                co_return 77;
            });
            REQUIRE(*b == 77);
            REQUIRE(attempt_ptr->load() == 2);
            co_return;
        });

        h.wait_destroyed();
    }

    sched.shutdown();
}

TEST_CASE("object_cache retries sweep startup after context allocation failure",
          "[object_cache][sweep][allocation][regression]") {
    scheduler sched(1);
    sched.start();

    {
        object_cache<std::string, int> cache;
        bool allocation_failed = false;
        int observed = 0;

        auto h = spawn_joinable(sched, [&]() -> task<void> {
            auto& fail_next_context_allocation =
                elio::coro::detail::
                    fail_next_task_execution_context_allocation_for_test;
            fail_next_context_allocation.store(
                true, std::memory_order_release);
            try {
                (void)co_await cache.get("first", []() -> task<int> {
                    co_return 1;
                });
            } catch (const std::bad_alloc&) {
                allocation_failed = true;
            }

            auto value = co_await cache.get("second", []() -> task<int> {
                co_return 2;
            });
            observed = *value;
        });

        h.wait_destroyed();
        REQUIRE_NOTHROW(h.await_resume());
        REQUIRE(allocation_failed);
        REQUIRE(observed == 2);
    }

    REQUIRE(sched.shutdown());
}

TEST_CASE("object_cache constructor destruction clears constructing entry",
          "[object_cache][construction][cancellation]") {
    using cache_type = object_cache<std::string, int>;

    scheduler sched(1);
    sched.start();

    {
        cache_type cache({.num_shards = 4});
        event unblock_constructor;
        std::atomic<bool> ctor_started{false};

        auto owner_task = [&]() -> task<void> {
            (void)co_await cache.get("cancel_key", [&]() -> task<int> {
                ctor_started.store(true, std::memory_order_release);
                co_await unblock_constructor.wait();
                co_return 1;
            });
            co_return;
        };

        auto owner = owner_task();
        auto h = elio::coro::detail::task_access::release(std::move(owner));
        h.resume();

        REQUIRE(ctor_started.load(std::memory_order_acquire));
        REQUIRE(cache.size() == 1);

        h.destroy();
        REQUIRE(cache.size() == 0);

        std::atomic<int> retry_ctors{0};
        int value = 0;
        auto retry = spawn_joinable(sched, [&]() -> task<void> {
            auto b = co_await cache.get("cancel_key", [&]() -> task<int> {
                retry_ctors.fetch_add(1, std::memory_order_relaxed);
                co_return 2;
            });
            value = *b;
            co_return;
        });
        retry.wait_destroyed();

        REQUIRE(retry_ctors.load(std::memory_order_relaxed) == 1);
        REQUIRE(value == 2);
    }

    sched.shutdown();
}

TEST_CASE("object_cache refcount and reclaim delay", "[object_cache]") {
    TrackedValue::reset_counts();

    scheduler sched(1);
    sched.start();

    {
        object_cache<std::string, TrackedValue> cache({
            .num_shards = 4,
            .reclaim_delay = std::chrono::milliseconds(50),
            .sweep_interval = std::chrono::milliseconds(20),
        });

        auto h = spawn_joinable(sched, [&]() -> task<void> {
            {
                auto b = co_await cache.get("tv1", []() -> task<TrackedValue> {
                    co_return TrackedValue(1);
                });
                REQUIRE(b->id == 1);
                REQUIRE(cache.size() == 1);
            }

            REQUIRE(cache.size() == 1);

            co_await elio::time::sleep_for(std::chrono::milliseconds(100));

            REQUIRE(cache.size() == 0);
            co_return;
        });

        h.wait_destroyed();
    }

    sched.shutdown();
}

TEST_CASE("object_cache re-borrow from reclaim queue", "[object_cache]") {
    scheduler sched(1);
    sched.start();

    std::atomic<int> ctor_calls{0};

    {
        object_cache<std::string, int> cache({
            .num_shards = 4,
            .reclaim_delay = std::chrono::milliseconds(200),
            .sweep_interval = std::chrono::milliseconds(50),
        });

        auto h = spawn_joinable(sched, [&]() -> task<void> {
            auto* cache_ptr = &cache;
            auto* ctor_ptr = &ctor_calls;

            {
                auto b = co_await cache_ptr->get("reborrow", [=]() -> task<int> {
                    ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_return 42;
                });
                REQUIRE(*b == 42);
            }

            co_await elio::time::sleep_for(std::chrono::milliseconds(30));

            {
                auto b2 = co_await cache_ptr->get("reborrow", [=]() -> task<int> {
                    ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_return 999;
                });
                REQUIRE(*b2 == 42);
            }

            co_return;
        });

        h.wait_destroyed();
    }

    REQUIRE(ctor_calls.load() == 1);
    sched.shutdown();
}

TEST_CASE("object_cache mark_evict", "[object_cache]") {
    scheduler sched(1);
    sched.start();

    std::atomic<int> ctor_calls{0};

    {
        object_cache<std::string, int> cache({.num_shards = 4});

        auto h = spawn_joinable(sched, [&]() -> task<void> {
            auto* cache_ptr = &cache;
            auto* ctor_ptr = &ctor_calls;

            auto b1 = co_await cache_ptr->get("evict_me", [=]() -> task<int> {
                ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                co_return 1;
            });
            REQUIRE(*b1 == 1);

            b1.mark_evict();

            auto b2 = co_await cache_ptr->get("evict_me", [=]() -> task<int> {
                ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                co_return 2;
            });
            REQUIRE(*b2 == 2);

            REQUIRE(*b1 == 1);
            co_return;
        });

        h.wait_destroyed();
    }

    REQUIRE(ctor_calls.load() == 2);
    sched.shutdown();
}

TEST_CASE("object_cache explicit evict", "[object_cache]") {
    scheduler sched(1);
    sched.start();

    std::atomic<int> ctor_calls{0};

    {
        object_cache<std::string, int> cache({.num_shards = 4});

        auto h = spawn_joinable(sched, [&]() -> task<void> {
            auto* cache_ptr = &cache;
            auto* ctor_ptr = &ctor_calls;

            {
                auto b = co_await cache_ptr->get("evkey", [=]() -> task<int> {
                    ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_return 10;
                });
                REQUIRE(*b == 10);
            }

            cache_ptr->evict("evkey");

            {
                auto b = co_await cache_ptr->get("evkey", [=]() -> task<int> {
                    ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_return 20;
                });
                REQUIRE(*b == 20);
            }

            co_return;
        });

        h.wait_destroyed();
    }

    REQUIRE(ctor_calls.load() == 2);
    sched.shutdown();
}

TEST_CASE("object_cache release ownership", "[object_cache]") {
    scheduler sched(1);
    sched.start();

    {
        object_cache<std::string, int> cache({.num_shards = 4});

        auto h = spawn_joinable(sched, [&]() -> task<void> {
            auto b = co_await cache.get("release_me", []() -> task<int> {
                co_return 42;
            });
            REQUIRE(*b == 42);

            auto owned = co_await b.release();
            REQUIRE(owned != nullptr);
            REQUIRE(*owned == 42);
            REQUIRE(!b);
            REQUIRE(cache.size() == 0);
            co_return;
        });

        h.wait_destroyed();
    }

    sched.shutdown();
}

TEST_CASE("object_cache release waits for other borrows", "[object_cache]") {
    scheduler sched(2);
    sched.start();

    std::atomic<bool> released{false};

    {
        object_cache<std::string, int> cache({.num_shards = 4});

        auto* cache_ptr = &cache;
        auto* released_ptr = &released;

        auto h1 = spawn_joinable(sched, [=]() -> task<void> {
            auto b = co_await cache_ptr->get("wait_key", []() -> task<int> {
                co_return 55;
            });

            co_await elio::time::sleep_for(std::chrono::milliseconds(50));
            REQUIRE(!released_ptr->load(std::memory_order_acquire));
            co_return;
        });

        auto h2 = spawn_joinable(sched, [=]() -> task<void> {
            co_await elio::time::sleep_for(std::chrono::milliseconds(5));
            auto b = co_await cache_ptr->get("wait_key", []() -> task<int> {
                co_return 999;
            });
            REQUIRE(*b == 55);

            auto owned = co_await b.release();
            released_ptr->store(true, std::memory_order_release);
            REQUIRE(*owned == 55);
            co_return;
        });

        h1.wait_destroyed();
        h2.wait_destroyed();
    }

    REQUIRE(released.load());
    sched.shutdown();
}

TEST_CASE("object_cache release handoff resumes once when final borrow drops after waiter publish",
          "[object_cache][release]") {
    scheduler sched(1);
    sched.start();

    using cache_type = object_cache<std::string, int>;

    std::atomic<int> hook_calls{0};
    std::atomic<int> release_continuations{0};
    std::optional<cache_type::borrow> other;

    struct hook_context {
        std::optional<cache_type::borrow>* other;
        std::atomic<int>* hook_calls;
    } ctx{&other, &hook_calls};

    struct hook_guard {
        hook_guard(void (*cb)(void*), void* ctx) {
            detail_oc::release_waiter_published_hook.context.store(
                ctx, std::memory_order_release);
            detail_oc::release_waiter_published_hook.callback.store(
                cb, std::memory_order_release);
        }

        ~hook_guard() {
            detail_oc::release_waiter_published_hook.callback.store(
                nullptr, std::memory_order_release);
            detail_oc::release_waiter_published_hook.context.store(
                nullptr, std::memory_order_release);
        }
    } guard{
        [](void* raw) noexcept {
            auto* c = static_cast<hook_context*>(raw);
            if (c->hook_calls->fetch_add(1, std::memory_order_acq_rel) == 0) {
                c->other->reset();
            }
        },
        &ctx};

    {
        cache_type cache({.num_shards = 4});

        auto h = spawn_joinable(sched, [&]() -> task<void> {
            auto releaser = co_await cache.get("race_key", []() -> task<int> {
                co_return 101;
            });
            other.emplace(co_await cache.get("race_key", []() -> task<int> {
                co_return 202;
            }));

            auto owned = co_await releaser.release();
            release_continuations.fetch_add(1, std::memory_order_relaxed);

            REQUIRE(owned != nullptr);
            REQUIRE(*owned == 101);
            REQUIRE(!releaser);
            co_return;
        });

        h.wait_destroyed();
    }

    REQUIRE(hook_calls.load(std::memory_order_acquire) == 1);
    REQUIRE(release_continuations.load(std::memory_order_acquire) == 1);

    sched.shutdown();
}

TEST_CASE("object_cache final shared drop always probes the release waiter slot",
          "[object_cache][release][handoff]") {
    detail_oc::release_waiter_probes_for_test.store(0,
                                                    std::memory_order_relaxed);

    scheduler sched(1);
    sched.start();

    {
        object_cache<std::string, int> cache({.num_shards = 4});
        auto h = spawn_joinable(sched, [&]() -> task<void> {
            auto first = co_await cache.get("handoff_key", []() -> task<int> {
                co_return 42;
            });
            std::optional<object_cache<std::string, int>::borrow> second;
            second.emplace(co_await cache.get(
                "handoff_key", []() -> task<int> { co_return 84; }));

            second.reset();
            REQUIRE(*first == 42);
            co_return;
        });

        h.wait_destroyed();
        REQUIRE_NOTHROW(h.await_resume());
        REQUIRE(detail_oc::release_waiter_probes_for_test.load(
                    std::memory_order_relaxed) == 1);
    }

    sched.shutdown();
}

TEST_CASE("object_cache release unregisters waiter when suspended release is destroyed",
          "[object_cache][release][cancellation]") {
    using cache_type = object_cache<std::string, int>;

    cache_type cache({.num_shards = 4});
    std::optional<cache_type::borrow> other;
    std::atomic<int> hook_calls{0};
    std::atomic<int> release_continuations{0};

    struct hook_context {
        std::atomic<int>* hook_calls;
    } ctx{&hook_calls};

    struct hook_guard {
        hook_guard(void (*cb)(void*), void* ctx) {
            detail_oc::release_waiter_published_hook.context.store(
                ctx, std::memory_order_release);
            detail_oc::release_waiter_published_hook.callback.store(
                cb, std::memory_order_release);
        }

        ~hook_guard() {
            detail_oc::release_waiter_published_hook.callback.store(
                nullptr, std::memory_order_release);
            detail_oc::release_waiter_published_hook.context.store(
                nullptr, std::memory_order_release);
        }
    } guard{
        [](void* raw) noexcept {
            auto* c = static_cast<hook_context*>(raw);
            c->hook_calls->fetch_add(1, std::memory_order_acq_rel);
        },
        &ctx};

    auto waiter_task = [&]() -> task<void> {
        auto releaser = co_await cache.get("cancel_key", []() -> task<int> {
            co_return 7;
        });
        other.emplace(co_await cache.get("cancel_key", []() -> task<int> {
            co_return 9;
        }));

        auto owned = co_await releaser.release();
        release_continuations.fetch_add(1, std::memory_order_relaxed);

        REQUIRE(owned != nullptr);
        REQUIRE(*owned == 7);
        co_return;
    };

    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(std::move(t));
    h.resume();

    REQUIRE(hook_calls.load(std::memory_order_acquire) == 1);
    REQUIRE(other.has_value());
    REQUIRE(release_continuations.load(std::memory_order_acquire) == 0);

    h.destroy();
    other.reset();

    REQUIRE(release_continuations.load(std::memory_order_acquire) == 0);
}

TEST_CASE("object_cache release skips a waiter destroyed after selection",
          "[object_cache][release][cancellation][lifetime][regression]") {
    using cache_type = object_cache<std::string, int>;
    using namespace elio::coro::detail;

    cache_type cache({.num_shards = 4});
    std::optional<cache_type::borrow> other;
    std::atomic<int> published{0};
    std::atomic<int> release_continuations{0};

    struct hook_context {
        std::atomic<int>* published;
    } ctx{&published};

    struct hook_guard {
        hook_guard(void (*cb)(void*), void* ctx) {
            detail_oc::release_waiter_published_hook.context.store(
                ctx, std::memory_order_release);
            detail_oc::release_waiter_published_hook.callback.store(
                cb, std::memory_order_release);
        }

        ~hook_guard() {
            detail_oc::release_waiter_published_hook.callback.store(
                nullptr, std::memory_order_release);
            detail_oc::release_waiter_published_hook.context.store(
                nullptr, std::memory_order_release);
        }
    } guard{
        [](void* raw) noexcept {
            auto* c = static_cast<hook_context*>(raw);
            c->published->fetch_add(1, std::memory_order_acq_rel);
        },
        &ctx};

    auto waiter_task = [&]() -> task<void> {
        auto releaser = co_await cache.get("selected_key", []() -> task<int> {
            co_return 17;
        });
        other.emplace(co_await cache.get(
            "selected_key", []() -> task<int> { co_return 19; }));

        auto owned = co_await releaser.release();
        release_continuations.fetch_add(1, std::memory_order_relaxed);
        REQUIRE(owned != nullptr);
    };

    auto waiter = waiter_task();
    auto handle = task_access::release(std::move(waiter));
    handle.resume();
    REQUIRE(published.load(std::memory_order_acquire) == 1);
    REQUIRE(other.has_value());
    REQUIRE(release_continuations.load(std::memory_order_acquire) == 0);

    completion_wake_claim_paused_for_test.store(false,
                                                std::memory_order_release);
    pause_before_completion_wake_claim_for_test.store(
        true, std::memory_order_release);
    std::thread producer([&other] { other.reset(); });

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (!completion_wake_claim_paused_for_test.load(
               std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool claim_paused = completion_wake_claim_paused_for_test.load(
        std::memory_order_acquire);
    if (claim_paused) {
        handle.destroy();
    }
    pause_before_completion_wake_claim_for_test.store(
        false, std::memory_order_release);
    pause_before_completion_wake_claim_for_test.notify_all();
    producer.join();

    completion_wake_claim_paused_for_test.store(false,
                                                std::memory_order_release);
    if (!claim_paused) {
        handle.destroy();
    }
    REQUIRE(claim_paused);
    REQUIRE_FALSE(other.has_value());
    REQUIRE(release_continuations.load(std::memory_order_acquire) == 0);
}

TEST_CASE("object_cache TTL expiry", "[object_cache]") {
    scheduler sched(1);
    sched.start();

    std::atomic<int> ctor_calls{0};

    {
        object_cache<std::string, int> cache({
            .num_shards = 4,
            .reclaim_delay = std::chrono::milliseconds(500),
            .sweep_interval = std::chrono::milliseconds(20),
            .default_ttl = std::chrono::milliseconds(50),
        });

        auto h = spawn_joinable(sched, [&]() -> task<void> {
            auto* cache_ptr = &cache;
            auto* ctor_ptr = &ctor_calls;

            {
                auto b = co_await cache_ptr->get("ttl_key", [=]() -> task<int> {
                    ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_return 1;
                });
                REQUIRE(*b == 1);
            }

            co_await elio::time::sleep_for(std::chrono::milliseconds(100));

            {
                auto b = co_await cache_ptr->get("ttl_key", [=]() -> task<int> {
                    ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_return 2;
                });
                REQUIRE(*b == 2);
            }

            co_return;
        });

        h.wait_destroyed();
    }

    REQUIRE(ctor_calls.load() == 2);
    sched.shutdown();
}

TEST_CASE("object_cache per-entry TTL", "[object_cache]") {
    scheduler sched(1);
    sched.start();

    std::atomic<int> ctor_calls{0};

    {
        object_cache<std::string, int> cache({
            .num_shards = 4,
            .sweep_interval = std::chrono::milliseconds(20),
        });

        auto h = spawn_joinable(sched, [&]() -> task<void> {
            auto* cache_ptr = &cache;
            auto* ctor_ptr = &ctor_calls;

            {
                auto b = co_await cache_ptr->get("short_ttl", [=]() -> task<int> {
                    ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_return 1;
                }, std::chrono::milliseconds(50));
                REQUIRE(*b == 1);
            }

            co_await elio::time::sleep_for(std::chrono::milliseconds(100));

            {
                auto b = co_await cache_ptr->get("short_ttl", [=]() -> task<int> {
                    ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_return 2;
                });
                REQUIRE(*b == 2);
            }

            co_return;
        });

        h.wait_destroyed();
    }

    REQUIRE(ctor_calls.load() == 2);
    sched.shutdown();
}

TEST_CASE("object_cache multi-key concurrent stress", "[object_cache][concurrent]") {
    scheduler sched(4);
    sched.start();

    std::atomic<int> total_borrows{0};

    {
        object_cache<int, int> cache({.num_shards = 16});

        constexpr int NUM_KEYS = 50;
        constexpr int NUM_TASKS = 100;
        std::vector<join_handle<void>> handles;

        auto* cache_ptr = &cache;
        auto* total_ptr = &total_borrows;

        for (int i = 0; i < NUM_TASKS; ++i) {
            handles.push_back(spawn_joinable(sched, [=]() -> task<void> {
                int key = i % NUM_KEYS;
                auto b = co_await cache_ptr->get(key, [key]() -> task<int> {
                    co_return key * 10;
                });
                REQUIRE(*b == key * 10);
                total_ptr->fetch_add(1, std::memory_order_relaxed);
                co_return;
            }));
        }

        for (auto& h : handles) h.wait_destroyed();
    }

    REQUIRE(total_borrows.load() == 100);
    sched.shutdown();
}

TEST_CASE("object_cache borrow move semantics", "[object_cache]") {
    scheduler sched(1);
    sched.start();

    {
        object_cache<std::string, int> cache({.num_shards = 4});

        auto h = spawn_joinable(sched, [&]() -> task<void> {
            auto b1 = co_await cache.get("move_key", []() -> task<int> {
                co_return 42;
            });
            REQUIRE(*b1 == 42);

            auto b2 = std::move(b1);
            REQUIRE(!b1);
            REQUIRE(*b2 == 42);

            decltype(b2) b3;
            b3 = std::move(b2);
            REQUIRE(!b2);
            REQUIRE(*b3 == 42);

            co_return;
        });

        h.wait_destroyed();
    }

    sched.shutdown();
}
