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
#include <exception>
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

namespace {

template<typename Handle>
bool wait_for_join_destruction(Handle& handle) {
    auto wait_until = [&](std::chrono::steady_clock::time_point deadline) {
        while (!handle.is_destroyed() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return handle.is_destroyed();
    };

    if (wait_until(std::chrono::steady_clock::now() +
                   elio::test::scaled_sec(5))) {
        return true;
    }

    handle.request_cancel();
    return wait_until(std::chrono::steady_clock::now() +
                      elio::test::scaled_sec(5));
}

template<typename HandleRange>
bool wait_for_join_destructions(HandleRange& handles) {
    auto all_destroyed = [&] {
        for (auto& handle : handles) {
            if (!handle.is_destroyed()) return false;
        }
        return true;
    };
    auto wait_until = [&](std::chrono::steady_clock::time_point deadline) {
        while (!all_destroyed() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return all_destroyed();
    };

    if (wait_until(std::chrono::steady_clock::now() +
                   elio::test::scaled_sec(5))) {
        return true;
    }

    for (auto& handle : handles) {
        if (!handle.is_destroyed()) handle.request_cancel();
    }
    return wait_until(std::chrono::steady_clock::now() +
                      elio::test::scaled_sec(5));
}

template<typename Handle>
bool settle_join_before_resource_release(scheduler& sched, Handle& handle,
                                         bool& scheduler_stopped,
                                         bool& shutdown_drained) {
    if (scheduler_stopped) {
        if (!handle.is_destroyed()) handle.wait_destroyed();
        return handle.is_destroyed();
    }

    if (wait_for_join_destruction(handle)) return true;

    shutdown_drained = sched.shutdown(elio::test::scaled_sec(5));
    scheduler_stopped = true;
    if (!handle.is_destroyed()) handle.wait_destroyed();
    return handle.is_destroyed();
}

template<typename HandleRange>
bool settle_joins_before_resource_release(scheduler& sched,
                                          HandleRange& handles,
                                          bool& scheduler_stopped,
                                          bool& shutdown_drained) {
    auto wait_without_timeout = [&] {
        for (auto& handle : handles) {
            if (!handle.is_destroyed()) handle.wait_destroyed();
        }
        for (auto& handle : handles) {
            if (!handle.is_destroyed()) return false;
        }
        return true;
    };

    if (scheduler_stopped) {
        return wait_without_timeout();
    }

    if (wait_for_join_destructions(handles)) return true;

    shutdown_drained = sched.shutdown(elio::test::scaled_sec(5));
    scheduler_stopped = true;
    return wait_without_timeout();
}

template<typename T>
struct join_completion {
    bool ready = false;
    bool destroyed = false;
    std::optional<T> value;
    std::exception_ptr exception;
};

template<typename T>
join_completion<T> collect_join_completion(join_handle<T>& handle) {
    join_completion<T> result;
    result.ready = handle.is_ready();
    result.destroyed = handle.is_destroyed();
    if (!result.ready || !result.destroyed) return result;

    try {
        result.value.emplace(handle.await_resume());
    } catch (...) {
        result.exception = std::current_exception();
    }
    return result;
}

void rethrow_join_exception(const std::exception_ptr& exception) {
    if (exception) std::rethrow_exception(exception);
}

} // namespace

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
    struct observation {
        int first = 0;
        int second = 0;
        int third = 0;
        size_t cache_size = 0;
    };

    scheduler sched(1);
    sched.start();

    bool destroyed = false;
    bool scheduler_stopped = false;
    bool shutdown = false;
    join_completion<observation> completion;

    {
        auto cache = std::make_shared<object_cache<std::string, int>>();

        auto h = spawn_joinable(sched, [cache]() -> task<observation> {
            auto b1 = co_await cache->get("key1", []() -> task<int> {
                co_return 42;
            });

            auto b2 = co_await cache->get("key1", []() -> task<int> {
                co_return 999;
            });

            auto b3 = co_await cache->get("key2", []() -> task<int> {
                co_return 100;
            });
            co_return observation{*b1, *b2, *b3, cache->size()};
        });

        destroyed = settle_join_before_resource_release(
            sched, h, scheduler_stopped, shutdown);
        completion = collect_join_completion(h);
    }

    if (!scheduler_stopped) {
        shutdown = sched.shutdown(elio::test::scaled_sec(5));
    }

    REQUIRE(destroyed);
    REQUIRE(shutdown);
    REQUIRE(completion.ready);
    REQUIRE(completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
    REQUIRE(completion.value.has_value());
    REQUIRE(completion.value->first == 42);
    REQUIRE(completion.value->second == 42);
    REQUIRE(completion.value->third == 100);
    REQUIRE(completion.value->cache_size == 2);
}

TEST_CASE("object_cache concurrent get deduplicates construction", "[object_cache]") {
    scheduler sched(4);
    sched.start();

    std::atomic<int> ctor_calls{0};
    std::atomic<int> completed{0};

    bool destroyed = false;
    bool scheduler_stopped = false;
    bool shutdown = false;
    std::vector<join_completion<int>> completions;

    {
        auto cache = std::make_shared<object_cache<std::string, int>>();

        constexpr int N = 20;
        std::vector<join_handle<int>> handles;

        auto* ctor_calls_ptr = &ctor_calls;
        auto* completed_ptr = &completed;

        for (int i = 0; i < N; ++i) {
            handles.push_back(spawn_joinable(sched, [=]() -> task<int> {
                auto b = co_await cache->get("shared_key", [=]() -> task<int> {
                    ctor_calls_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_await elio::time::sleep_for(std::chrono::milliseconds(10));
                    co_return 42;
                });
                completed_ptr->fetch_add(1, std::memory_order_relaxed);
                co_return *b;
            }));
        }

        destroyed = settle_joins_before_resource_release(
            sched, handles, scheduler_stopped, shutdown);
        completions.reserve(handles.size());
        for (auto& handle : handles) {
            completions.push_back(collect_join_completion(handle));
        }
    }

    if (!scheduler_stopped) {
        shutdown = sched.shutdown(elio::test::scaled_sec(5));
    }

    REQUIRE(destroyed);
    REQUIRE(shutdown);
    REQUIRE(ctor_calls.load() == 1);
    REQUIRE(completed.load() == 20);
    REQUIRE(completions.size() == 20);
    for (size_t index = 0; index < completions.size(); ++index) {
        CAPTURE(index);
        const auto& completion = completions[index];
        REQUIRE(completion.ready);
        REQUIRE(completion.destroyed);
        REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
        REQUIRE(completion.value.has_value());
        REQUIRE(*completion.value == 42);
    }
}

TEST_CASE("object_cache construction failure and retry", "[object_cache]") {
    scheduler sched(1);
    sched.start();

    std::atomic<int> attempt{0};

    struct observation {
        bool caught = false;
        int value = 0;
        int attempts = 0;
    };
    bool destroyed = false;
    bool scheduler_stopped = false;
    bool shutdown = false;
    join_completion<observation> completion;

    {
        auto cache = std::make_shared<object_cache<std::string, int>>();

        auto h = spawn_joinable(sched, [cache, &attempt]() -> task<observation> {
            auto* attempt_ptr = &attempt;

            bool caught = false;
            try {
                co_await cache->get("fail_key", [=]() -> task<int> {
                    attempt_ptr->fetch_add(1, std::memory_order_relaxed);
                    throw std::runtime_error("construction failed");
                    co_return 0;
                });
            } catch (const std::runtime_error&) {
                caught = true;
            }

            auto b = co_await cache->get("fail_key", [=]() -> task<int> {
                attempt_ptr->fetch_add(1, std::memory_order_relaxed);
                co_return 77;
            });
            co_return observation{caught, *b, attempt_ptr->load()};
        });

        destroyed = settle_join_before_resource_release(
            sched, h, scheduler_stopped, shutdown);
        completion = collect_join_completion(h);
    }

    if (!scheduler_stopped) {
        shutdown = sched.shutdown(elio::test::scaled_sec(5));
    }

    REQUIRE(destroyed);
    REQUIRE(shutdown);
    REQUIRE(completion.ready);
    REQUIRE(completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
    REQUIRE(completion.value.has_value());
    REQUIRE(completion.value->caught);
    REQUIRE(completion.value->value == 77);
    REQUIRE(completion.value->attempts == 2);
}

TEST_CASE("object_cache retries sweep startup after context allocation failure",
          "[object_cache][sweep][allocation][regression]") {
    struct observation {
        bool allocation_failed = false;
        int value = 0;
    };

    scheduler sched(1);
    sched.start();

    bool destroyed = false;
    bool scheduler_stopped = false;
    bool shutdown = false;
    join_completion<observation> completion;

    {
        auto cache = std::make_shared<object_cache<std::string, int>>();

        auto h = spawn_joinable(sched, [cache]() -> task<observation> {
            bool allocation_failed = false;
            auto& fail_next_context_allocation =
                elio::coro::detail::
                    fail_next_task_execution_context_allocation_for_test;
            fail_next_context_allocation.store(
                true, std::memory_order_release);
            try {
                (void)co_await cache->get("first", []() -> task<int> {
                    co_return 1;
                });
            } catch (const std::bad_alloc&) {
                allocation_failed = true;
            }

            auto value = co_await cache->get("second", []() -> task<int> {
                co_return 2;
            });
            co_return observation{allocation_failed, *value};
        });

        destroyed = settle_join_before_resource_release(
            sched, h, scheduler_stopped, shutdown);
        completion = collect_join_completion(h);
    }

    if (!scheduler_stopped) {
        shutdown = sched.shutdown(elio::test::scaled_sec(5));
    }

    REQUIRE(destroyed);
    REQUIRE(shutdown);
    REQUIRE(completion.ready);
    REQUIRE(completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
    REQUIRE(completion.value.has_value());
    REQUIRE(completion.value->allocation_failed);
    REQUIRE(completion.value->value == 2);
}

TEST_CASE("object_cache constructor destruction clears constructing entry",
          "[object_cache][construction][cancellation]") {
    using cache_type = object_cache<std::string, int>;

    scheduler sched(1);
    sched.start();

    bool constructor_started = false;
    size_t size_while_constructing = 0;
    size_t size_after_destroy = 0;
    bool retry_destroyed = false;
    bool scheduler_stopped = false;
    bool shutdown = false;
    join_completion<int> retry_completion;
    int retry_constructor_calls = 0;

    {
        auto cache = std::make_shared<cache_type>(
            object_cache_config{.num_shards = 4});
        event unblock_constructor;
        std::atomic<bool> ctor_started{false};

        auto owner_task = [&, cache]() -> task<void> {
            (void)co_await cache->get("cancel_key", [&]() -> task<int> {
                ctor_started.store(true, std::memory_order_release);
                co_await unblock_constructor.wait();
                co_return 1;
            });
            co_return;
        };

        auto owner = owner_task();
        auto h = elio::coro::detail::task_access::release(std::move(owner));
        h.resume();

        constructor_started = ctor_started.load(std::memory_order_acquire);
        size_while_constructing = cache->size();

        h.destroy();
        size_after_destroy = cache->size();

        std::atomic<int> retry_ctors{0};
        auto retry = spawn_joinable(sched, [&, cache]() -> task<int> {
            auto b = co_await cache->get("cancel_key", [&]() -> task<int> {
                retry_ctors.fetch_add(1, std::memory_order_relaxed);
                co_return 2;
            });
            co_return *b;
        });
        retry_destroyed = settle_join_before_resource_release(
            sched, retry, scheduler_stopped, shutdown);
        retry_completion = collect_join_completion(retry);
        retry_constructor_calls = retry_ctors.load(std::memory_order_relaxed);
    }

    if (!scheduler_stopped) {
        shutdown = sched.shutdown(elio::test::scaled_sec(5));
    }

    REQUIRE(constructor_started);
    REQUIRE(size_while_constructing == 1);
    REQUIRE(size_after_destroy == 0);
    REQUIRE(retry_destroyed);
    REQUIRE(shutdown);
    REQUIRE(retry_completion.ready);
    REQUIRE(retry_completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(retry_completion.exception));
    REQUIRE(retry_completion.value.has_value());
    REQUIRE(*retry_completion.value == 2);
    REQUIRE(retry_constructor_calls == 1);
}

TEST_CASE("object_cache refcount and reclaim delay", "[object_cache]") {
    struct observation {
        int id = 0;
        size_t size_while_borrowed = 0;
        size_t size_after_release = 0;
        size_t size_after_delay = 0;
    };

    TrackedValue::reset_counts();

    scheduler sched(1);
    sched.start();

    bool destroyed = false;
    bool scheduler_stopped = false;
    bool shutdown = false;
    join_completion<observation> completion;

    {
        auto cache = std::make_shared<object_cache<std::string, TrackedValue>>(
            object_cache_config{
            .num_shards = 4,
            .reclaim_delay = std::chrono::milliseconds(50),
            .sweep_interval = std::chrono::milliseconds(20),
        });

        auto h = spawn_joinable(sched, [cache]() -> task<observation> {
            observation observed;
            {
                auto b = co_await cache->get("tv1", []() -> task<TrackedValue> {
                    co_return TrackedValue(1);
                });
                observed.id = b->id;
                observed.size_while_borrowed = cache->size();
            }

            observed.size_after_release = cache->size();

            co_await elio::time::sleep_for(std::chrono::milliseconds(100));

            observed.size_after_delay = cache->size();
            co_return observed;
        });

        destroyed = settle_join_before_resource_release(
            sched, h, scheduler_stopped, shutdown);
        completion = collect_join_completion(h);
    }

    if (!scheduler_stopped) {
        shutdown = sched.shutdown(elio::test::scaled_sec(5));
    }

    REQUIRE(destroyed);
    REQUIRE(shutdown);
    REQUIRE(completion.ready);
    REQUIRE(completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
    REQUIRE(completion.value.has_value());
    REQUIRE(completion.value->id == 1);
    REQUIRE(completion.value->size_while_borrowed == 1);
    REQUIRE(completion.value->size_after_release == 1);
    REQUIRE(completion.value->size_after_delay == 0);
}

TEST_CASE("object_cache re-borrow from reclaim queue", "[object_cache]") {
    struct observation {
        int first = 0;
        int second = 0;
    };

    scheduler sched(1);
    sched.start();

    std::atomic<int> ctor_calls{0};
    bool destroyed = false;
    bool scheduler_stopped = false;
    bool shutdown = false;
    join_completion<observation> completion;

    {
        auto cache = std::make_shared<object_cache<std::string, int>>(
            object_cache_config{
            .num_shards = 4,
            .reclaim_delay = std::chrono::milliseconds(200),
            .sweep_interval = std::chrono::milliseconds(50),
        });

        auto h = spawn_joinable(sched, [cache, &ctor_calls]() -> task<observation> {
            auto* ctor_ptr = &ctor_calls;
            observation observed;

            {
                auto b = co_await cache->get("reborrow", [=]() -> task<int> {
                    ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_return 42;
                });
                observed.first = *b;
            }

            co_await elio::time::sleep_for(std::chrono::milliseconds(30));

            {
                auto b2 = co_await cache->get("reborrow", [=]() -> task<int> {
                    ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_return 999;
                });
                observed.second = *b2;
            }

            co_return observed;
        });

        destroyed = settle_join_before_resource_release(
            sched, h, scheduler_stopped, shutdown);
        completion = collect_join_completion(h);
    }

    if (!scheduler_stopped) {
        shutdown = sched.shutdown(elio::test::scaled_sec(5));
    }

    REQUIRE(destroyed);
    REQUIRE(shutdown);
    REQUIRE(completion.ready);
    REQUIRE(completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
    REQUIRE(completion.value.has_value());
    REQUIRE(completion.value->first == 42);
    REQUIRE(completion.value->second == 42);
    REQUIRE(ctor_calls.load() == 1);
}

TEST_CASE("object_cache mark_evict", "[object_cache]") {
    struct observation {
        int first_before_evict = 0;
        int replacement = 0;
        int first_after_evict = 0;
    };

    scheduler sched(1);
    sched.start();

    std::atomic<int> ctor_calls{0};
    bool destroyed = false;
    bool scheduler_stopped = false;
    bool shutdown = false;
    join_completion<observation> completion;

    {
        auto cache = std::make_shared<object_cache<std::string, int>>(
            object_cache_config{.num_shards = 4});

        auto h = spawn_joinable(sched, [cache, &ctor_calls]() -> task<observation> {
            auto* ctor_ptr = &ctor_calls;

            auto b1 = co_await cache->get("evict_me", [=]() -> task<int> {
                ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                co_return 1;
            });
            const int first_before_evict = *b1;

            b1.mark_evict();

            auto b2 = co_await cache->get("evict_me", [=]() -> task<int> {
                ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                co_return 2;
            });
            co_return observation{first_before_evict, *b2, *b1};
        });

        destroyed = settle_join_before_resource_release(
            sched, h, scheduler_stopped, shutdown);
        completion = collect_join_completion(h);
    }

    if (!scheduler_stopped) {
        shutdown = sched.shutdown(elio::test::scaled_sec(5));
    }

    REQUIRE(destroyed);
    REQUIRE(shutdown);
    REQUIRE(completion.ready);
    REQUIRE(completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
    REQUIRE(completion.value.has_value());
    REQUIRE(completion.value->first_before_evict == 1);
    REQUIRE(completion.value->replacement == 2);
    REQUIRE(completion.value->first_after_evict == 1);
    REQUIRE(ctor_calls.load() == 2);
}

TEST_CASE("object_cache explicit evict", "[object_cache]") {
    struct observation {
        int first = 0;
        int replacement = 0;
    };

    scheduler sched(1);
    sched.start();

    std::atomic<int> ctor_calls{0};
    bool destroyed = false;
    bool scheduler_stopped = false;
    bool shutdown = false;
    join_completion<observation> completion;

    {
        auto cache = std::make_shared<object_cache<std::string, int>>(
            object_cache_config{.num_shards = 4});

        auto h = spawn_joinable(sched, [cache, &ctor_calls]() -> task<observation> {
            auto* ctor_ptr = &ctor_calls;
            observation observed;

            {
                auto b = co_await cache->get("evkey", [=]() -> task<int> {
                    ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_return 10;
                });
                observed.first = *b;
            }

            cache->evict("evkey");

            {
                auto b = co_await cache->get("evkey", [=]() -> task<int> {
                    ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_return 20;
                });
                observed.replacement = *b;
            }

            co_return observed;
        });

        destroyed = settle_join_before_resource_release(
            sched, h, scheduler_stopped, shutdown);
        completion = collect_join_completion(h);
    }

    if (!scheduler_stopped) {
        shutdown = sched.shutdown(elio::test::scaled_sec(5));
    }

    REQUIRE(destroyed);
    REQUIRE(shutdown);
    REQUIRE(completion.ready);
    REQUIRE(completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
    REQUIRE(completion.value.has_value());
    REQUIRE(completion.value->first == 10);
    REQUIRE(completion.value->replacement == 20);
    REQUIRE(ctor_calls.load() == 2);
}

TEST_CASE("object_cache release ownership", "[object_cache]") {
    struct observation {
        int borrowed_value = 0;
        bool owned = false;
        int owned_value = 0;
        bool borrow_empty = false;
        size_t cache_size = 0;
    };

    scheduler sched(1);
    sched.start();

    bool destroyed = false;
    bool scheduler_stopped = false;
    bool shutdown = false;
    join_completion<observation> completion;

    {
        auto cache = std::make_shared<object_cache<std::string, int>>(
            object_cache_config{.num_shards = 4});

        auto h = spawn_joinable(sched, [cache]() -> task<observation> {
            auto b = co_await cache->get("release_me", []() -> task<int> {
                co_return 42;
            });
            const int borrowed_value = *b;

            auto owned = co_await b.release();
            co_return observation{
                borrowed_value,
                owned != nullptr,
                owned ? *owned : 0,
                !b,
                cache->size()};
        });

        destroyed = settle_join_before_resource_release(
            sched, h, scheduler_stopped, shutdown);
        completion = collect_join_completion(h);
    }

    if (!scheduler_stopped) {
        shutdown = sched.shutdown(elio::test::scaled_sec(5));
    }

    REQUIRE(destroyed);
    REQUIRE(shutdown);
    REQUIRE(completion.ready);
    REQUIRE(completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
    REQUIRE(completion.value.has_value());
    REQUIRE(completion.value->borrowed_value == 42);
    REQUIRE(completion.value->owned);
    REQUIRE(completion.value->owned_value == 42);
    REQUIRE(completion.value->borrow_empty);
    REQUIRE(completion.value->cache_size == 0);
}

TEST_CASE("object_cache release waits for other borrows", "[object_cache]") {
    struct release_observation {
        int borrowed_value = 0;
        int owned_value = 0;
    };

    scheduler sched(2);
    sched.start();

    std::atomic<bool> released{false};
    bool first_destroyed = false;
    bool second_destroyed = false;
    bool scheduler_stopped = false;
    bool shutdown = false;
    join_completion<bool> first_completion;
    join_completion<release_observation> second_completion;

    {
        auto cache = std::make_shared<object_cache<std::string, int>>(
            object_cache_config{.num_shards = 4});

        auto* released_ptr = &released;

        auto h1 = spawn_joinable(sched, [=]() -> task<bool> {
            auto b = co_await cache->get("wait_key", []() -> task<int> {
                co_return 55;
            });

            co_await elio::time::sleep_for(std::chrono::milliseconds(50));
            co_return !released_ptr->load(std::memory_order_acquire);
        });

        auto h2 = spawn_joinable(sched, [=]() -> task<release_observation> {
            co_await elio::time::sleep_for(std::chrono::milliseconds(5));
            auto b = co_await cache->get("wait_key", []() -> task<int> {
                co_return 999;
            });
            const int borrowed_value = *b;

            auto owned = co_await b.release();
            released_ptr->store(true, std::memory_order_release);
            co_return release_observation{
                borrowed_value, owned ? *owned : 0};
        });

        first_destroyed = settle_join_before_resource_release(
            sched, h1, scheduler_stopped, shutdown);
        second_destroyed = settle_join_before_resource_release(
            sched, h2, scheduler_stopped, shutdown);
        first_completion = collect_join_completion(h1);
        second_completion = collect_join_completion(h2);
    }

    if (!scheduler_stopped) {
        shutdown = sched.shutdown(elio::test::scaled_sec(5));
    }

    REQUIRE(first_destroyed);
    REQUIRE(second_destroyed);
    REQUIRE(shutdown);
    REQUIRE(first_completion.ready);
    REQUIRE(first_completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(first_completion.exception));
    REQUIRE(first_completion.value.has_value());
    REQUIRE(*first_completion.value);
    REQUIRE(second_completion.ready);
    REQUIRE(second_completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(second_completion.exception));
    REQUIRE(second_completion.value.has_value());
    REQUIRE(second_completion.value->borrowed_value == 55);
    REQUIRE(second_completion.value->owned_value == 55);
    REQUIRE(released.load());
}

TEST_CASE("object_cache release handoff resumes once when final borrow drops after waiter publish",
          "[object_cache][release]") {
    scheduler sched(1);
    sched.start();

    using cache_type = object_cache<std::string, int>;

    std::atomic<int> hook_calls{0};
    std::atomic<int> release_continuations{0};
    std::optional<cache_type::borrow> other;

    struct observation {
        bool owned = false;
        int owned_value = 0;
        bool releaser_empty = false;
    };
    bool destroyed = false;
    bool scheduler_stopped = false;
    bool shutdown = false;
    join_completion<observation> completion;

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
        auto cache = std::make_shared<cache_type>(
            object_cache_config{.num_shards = 4});

        auto h = spawn_joinable(sched, [&, cache]() -> task<observation> {
            auto releaser = co_await cache->get("race_key", []() -> task<int> {
                co_return 101;
            });
            other.emplace(co_await cache->get("race_key", []() -> task<int> {
                co_return 202;
            }));

            auto owned = co_await releaser.release();
            release_continuations.fetch_add(1, std::memory_order_relaxed);

            co_return observation{
                owned != nullptr, owned ? *owned : 0, !releaser};
        });

        destroyed = settle_join_before_resource_release(
            sched, h, scheduler_stopped, shutdown);
        completion = collect_join_completion(h);
    }

    if (!scheduler_stopped) {
        shutdown = sched.shutdown(elio::test::scaled_sec(5));
    }

    REQUIRE(destroyed);
    REQUIRE(shutdown);
    REQUIRE(completion.ready);
    REQUIRE(completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
    REQUIRE(completion.value.has_value());
    REQUIRE(completion.value->owned);
    REQUIRE(completion.value->owned_value == 101);
    REQUIRE(completion.value->releaser_empty);
    REQUIRE(hook_calls.load(std::memory_order_acquire) == 1);
    REQUIRE(release_continuations.load(std::memory_order_acquire) == 1);
}

TEST_CASE("object_cache final shared drop always probes the release waiter slot",
          "[object_cache][release][handoff]") {
    detail_oc::release_waiter_probes_for_test.store(0,
                                                    std::memory_order_relaxed);

    scheduler sched(1);
    sched.start();

    bool destroyed = false;
    bool scheduler_stopped = false;
    bool shutdown = false;
    join_completion<int> completion;
    size_t probe_count = 0;

    {
        auto cache = std::make_shared<object_cache<std::string, int>>(
            object_cache_config{.num_shards = 4});
        auto h = spawn_joinable(sched, [cache]() -> task<int> {
            auto first = co_await cache->get("handoff_key", []() -> task<int> {
                co_return 42;
            });
            std::optional<object_cache<std::string, int>::borrow> second;
            second.emplace(co_await cache->get(
                "handoff_key", []() -> task<int> { co_return 84; }));

            second.reset();
            co_return *first;
        });

        destroyed = settle_join_before_resource_release(
            sched, h, scheduler_stopped, shutdown);
        completion = collect_join_completion(h);
        probe_count = detail_oc::release_waiter_probes_for_test.load(
            std::memory_order_relaxed);
    }

    if (!scheduler_stopped) {
        shutdown = sched.shutdown(elio::test::scaled_sec(5));
    }

    REQUIRE(destroyed);
    REQUIRE(shutdown);
    REQUIRE(completion.ready);
    REQUIRE(completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
    REQUIRE(completion.value.has_value());
    REQUIRE(*completion.value == 42);
    REQUIRE(probe_count == 1);
}

TEST_CASE("object_cache release unregisters waiter when suspended release is destroyed",
          "[object_cache][release][cancellation]") {
    using cache_type = object_cache<std::string, int>;

    cache_type cache({.num_shards = 4});
    std::optional<cache_type::borrow> other;
    std::atomic<int> hook_calls{0};
    std::atomic<int> release_continuations{0};
    bool continuation_owned = false;
    int continuation_value = 0;

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
        continuation_owned = owned != nullptr;
        continuation_value = owned ? *owned : 0;
        co_return;
    };

    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(std::move(t));
    h.resume();

    const int hooks_before_destroy = hook_calls.load(std::memory_order_acquire);
    const bool borrow_before_destroy = other.has_value();
    const int continuations_before_destroy =
        release_continuations.load(std::memory_order_acquire);

    h.destroy();
    other.reset();

    const int continuations_after_cleanup =
        release_continuations.load(std::memory_order_acquire);

    CAPTURE(continuation_owned, continuation_value);
    REQUIRE(hooks_before_destroy == 1);
    REQUIRE(borrow_before_destroy);
    REQUIRE(continuations_before_destroy == 0);
    if (continuations_after_cleanup != 0) {
        REQUIRE(continuation_owned);
        REQUIRE(continuation_value == 7);
    }
    REQUIRE(continuations_after_cleanup == 0);
}

TEST_CASE("object_cache release skips a waiter destroyed after selection",
          "[object_cache][release][cancellation][lifetime][regression]") {
    using cache_type = object_cache<std::string, int>;
    using namespace elio::coro::detail;

    struct race_state {
        race_state()
            : cache(object_cache_config{.num_shards = 4}) {}

        cache_type cache;
        std::optional<cache_type::borrow> other;
        std::atomic<int> published{0};
        std::atomic<int> release_continuations{0};
        std::atomic<bool> continuation_owned{false};
        std::atomic<bool> producer_done{false};
    };
    auto state = std::make_shared<race_state>();

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
            auto* state = static_cast<race_state*>(raw);
            state->published.fetch_add(1, std::memory_order_acq_rel);
        },
        state.get()};

    auto waiter_task = [](std::shared_ptr<race_state> state) -> task<void> {
        auto releaser = co_await state->cache.get(
            "selected_key", []() -> task<int> { co_return 17; });
        state->other.emplace(co_await state->cache.get(
            "selected_key", []() -> task<int> { co_return 19; }));

        auto owned = co_await releaser.release();
        state->release_continuations.fetch_add(1, std::memory_order_relaxed);
        state->continuation_owned.store(
            owned != nullptr, std::memory_order_release);
    };

    auto waiter = waiter_task(state);
    auto handle = task_access::release(std::move(waiter));
    handle.resume();
    const int published_before_selection =
        state->published.load(std::memory_order_acquire);
    const bool borrow_before_selection = state->other.has_value();
    const int continuations_before_selection =
        state->release_continuations.load(std::memory_order_acquire);

    completion_wake_claim_paused_for_test.store(false,
                                                std::memory_order_release);
    pause_before_completion_wake_claim_for_test.store(
        true, std::memory_order_release);
    std::thread producer([state] {
        state->other.reset();
        state->producer_done.store(true, std::memory_order_release);
        state->producer_done.notify_all();
    });

    auto deadline = std::chrono::steady_clock::now() +
                    elio::test::scaled_sec(5);
    while (!completion_wake_claim_paused_for_test.load(
               std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool claim_paused = completion_wake_claim_paused_for_test.load(
        std::memory_order_acquire);
    bool handle_destroyed_by_test = false;
    if (claim_paused) {
        handle.destroy();
        handle_destroyed_by_test = true;
    }
    pause_before_completion_wake_claim_for_test.store(
        false, std::memory_order_release);
    pause_before_completion_wake_claim_for_test.notify_all();

    producer.join();
    const bool producer_completed =
        state->producer_done.load(std::memory_order_acquire);
    if (!handle_destroyed_by_test) {
        handle.destroy();
        handle_destroyed_by_test = true;
    }

    completion_wake_claim_paused_for_test.store(false,
                                                std::memory_order_release);
    const bool borrow_after_cleanup = state->other.has_value();
    const int continuations_after_cleanup =
        state->release_continuations.load(std::memory_order_acquire);
    const bool continuation_owned =
        state->continuation_owned.load(std::memory_order_acquire);

    CAPTURE(continuation_owned, handle_destroyed_by_test);
    REQUIRE(published_before_selection == 1);
    REQUIRE(borrow_before_selection);
    REQUIRE(continuations_before_selection == 0);
    REQUIRE(producer_completed);
    REQUIRE(claim_paused);
    REQUIRE_FALSE(borrow_after_cleanup);
    if (continuations_after_cleanup != 0) {
        REQUIRE(continuation_owned);
    }
    REQUIRE(continuations_after_cleanup == 0);
}

TEST_CASE("object_cache TTL expiry", "[object_cache]") {
    struct observation {
        int initial = 0;
        int refreshed = 0;
    };

    scheduler sched(1);
    sched.start();

    std::atomic<int> ctor_calls{0};
    bool destroyed = false;
    bool scheduler_stopped = false;
    bool shutdown = false;
    join_completion<observation> completion;

    {
        auto cache = std::make_shared<object_cache<std::string, int>>(
            object_cache_config{
            .num_shards = 4,
            .reclaim_delay = std::chrono::milliseconds(500),
            .sweep_interval = std::chrono::milliseconds(20),
            .default_ttl = std::chrono::milliseconds(50),
        });

        auto h = spawn_joinable(sched, [cache, &ctor_calls]() -> task<observation> {
            auto* ctor_ptr = &ctor_calls;
            observation observed;

            {
                auto b = co_await cache->get("ttl_key", [=]() -> task<int> {
                    ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_return 1;
                });
                observed.initial = *b;
            }

            co_await elio::time::sleep_for(std::chrono::milliseconds(100));

            {
                auto b = co_await cache->get("ttl_key", [=]() -> task<int> {
                    ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_return 2;
                });
                observed.refreshed = *b;
            }

            co_return observed;
        });

        destroyed = settle_join_before_resource_release(
            sched, h, scheduler_stopped, shutdown);
        completion = collect_join_completion(h);
    }

    if (!scheduler_stopped) {
        shutdown = sched.shutdown(elio::test::scaled_sec(5));
    }

    REQUIRE(destroyed);
    REQUIRE(shutdown);
    REQUIRE(completion.ready);
    REQUIRE(completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
    REQUIRE(completion.value.has_value());
    REQUIRE(completion.value->initial == 1);
    REQUIRE(completion.value->refreshed == 2);
    REQUIRE(ctor_calls.load() == 2);
}

TEST_CASE("object_cache per-entry TTL", "[object_cache]") {
    struct observation {
        int initial = 0;
        int refreshed = 0;
    };

    scheduler sched(1);
    sched.start();

    std::atomic<int> ctor_calls{0};
    bool destroyed = false;
    bool scheduler_stopped = false;
    bool shutdown = false;
    join_completion<observation> completion;

    {
        auto cache = std::make_shared<object_cache<std::string, int>>(
            object_cache_config{
            .num_shards = 4,
            .sweep_interval = std::chrono::milliseconds(20),
        });

        auto h = spawn_joinable(sched, [cache, &ctor_calls]() -> task<observation> {
            auto* ctor_ptr = &ctor_calls;
            observation observed;

            {
                auto b = co_await cache->get("short_ttl", [=]() -> task<int> {
                    ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_return 1;
                }, std::chrono::milliseconds(50));
                observed.initial = *b;
            }

            co_await elio::time::sleep_for(std::chrono::milliseconds(100));

            {
                auto b = co_await cache->get("short_ttl", [=]() -> task<int> {
                    ctor_ptr->fetch_add(1, std::memory_order_relaxed);
                    co_return 2;
                });
                observed.refreshed = *b;
            }

            co_return observed;
        });

        destroyed = settle_join_before_resource_release(
            sched, h, scheduler_stopped, shutdown);
        completion = collect_join_completion(h);
    }

    if (!scheduler_stopped) {
        shutdown = sched.shutdown(elio::test::scaled_sec(5));
    }

    REQUIRE(destroyed);
    REQUIRE(shutdown);
    REQUIRE(completion.ready);
    REQUIRE(completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
    REQUIRE(completion.value.has_value());
    REQUIRE(completion.value->initial == 1);
    REQUIRE(completion.value->refreshed == 2);
    REQUIRE(ctor_calls.load() == 2);
}

TEST_CASE("object_cache multi-key concurrent stress", "[object_cache][concurrent]") {
    struct observation {
        int key = 0;
        int value = 0;
    };

    scheduler sched(4);
    sched.start();

    std::atomic<int> total_borrows{0};
    bool destroyed = false;
    bool scheduler_stopped = false;
    bool shutdown = false;
    std::vector<join_completion<observation>> completions;

    {
        auto cache = std::make_shared<object_cache<int, int>>(
            object_cache_config{.num_shards = 16});

        constexpr int NUM_KEYS = 50;
        constexpr int NUM_TASKS = 100;
        std::vector<join_handle<observation>> handles;

        auto* total_ptr = &total_borrows;

        for (int i = 0; i < NUM_TASKS; ++i) {
            handles.push_back(spawn_joinable(sched, [=]() -> task<observation> {
                int key = i % NUM_KEYS;
                auto b = co_await cache->get(key, [key]() -> task<int> {
                    co_return key * 10;
                });
                total_ptr->fetch_add(1, std::memory_order_relaxed);
                co_return observation{key, *b};
            }));
        }

        destroyed = settle_joins_before_resource_release(
            sched, handles, scheduler_stopped, shutdown);
        completions.reserve(handles.size());
        for (auto& handle : handles) {
            completions.push_back(collect_join_completion(handle));
        }
    }

    if (!scheduler_stopped) {
        shutdown = sched.shutdown(elio::test::scaled_sec(5));
    }

    REQUIRE(destroyed);
    REQUIRE(shutdown);
    REQUIRE(total_borrows.load() == 100);
    REQUIRE(completions.size() == 100);
    for (size_t index = 0; index < completions.size(); ++index) {
        CAPTURE(index);
        const auto& completion = completions[index];
        REQUIRE(completion.ready);
        REQUIRE(completion.destroyed);
        REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
        REQUIRE(completion.value.has_value());
        REQUIRE(completion.value->value == completion.value->key * 10);
    }
}

TEST_CASE("object_cache borrow move semantics", "[object_cache]") {
    struct observation {
        int first = 0;
        bool first_empty = false;
        int second = 0;
        bool second_empty = false;
        int third = 0;
    };

    scheduler sched(1);
    sched.start();

    bool destroyed = false;
    bool scheduler_stopped = false;
    bool shutdown = false;
    join_completion<observation> completion;

    {
        auto cache = std::make_shared<object_cache<std::string, int>>(
            object_cache_config{.num_shards = 4});

        auto h = spawn_joinable(sched, [cache]() -> task<observation> {
            auto b1 = co_await cache->get("move_key", []() -> task<int> {
                co_return 42;
            });
            const int first = *b1;

            auto b2 = std::move(b1);
            const bool first_empty = !b1;
            const int second = *b2;

            decltype(b2) b3;
            b3 = std::move(b2);
            co_return observation{first, first_empty, second, !b2, *b3};
        });

        destroyed = settle_join_before_resource_release(
            sched, h, scheduler_stopped, shutdown);
        completion = collect_join_completion(h);
    }

    if (!scheduler_stopped) {
        shutdown = sched.shutdown(elio::test::scaled_sec(5));
    }

    REQUIRE(destroyed);
    REQUIRE(shutdown);
    REQUIRE(completion.ready);
    REQUIRE(completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
    REQUIRE(completion.value.has_value());
    REQUIRE(completion.value->first == 42);
    REQUIRE(completion.value->first_empty);
    REQUIRE(completion.value->second == 42);
    REQUIRE(completion.value->second_empty);
    REQUIRE(completion.value->third == 42);
}
