#include <catch2/catch_test_macros.hpp>
#define ELIO_OBJECT_CACHE_TEST_HOOKS 1
#include <elio/sync/object_cache.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>

#include <atomic>
#include <optional>
#include <string>
#include <thread>
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
    auto h = elio::coro::detail::task_access::release(t);
    h.resume();

    REQUIRE(hook_calls.load(std::memory_order_acquire) == 1);
    REQUIRE(other.has_value());
    REQUIRE(release_continuations.load(std::memory_order_acquire) == 0);

    h.destroy();
    other.reset();

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
