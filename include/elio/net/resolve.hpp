#pragma once

#include <elio/net/tcp.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>

#include <netdb.h>
#include <net/if.h>
#include <arpa/inet.h>

#include <atomic>
#include <array>
#include <chrono>
#include <coroutine>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace elio::net {

struct resolve_cache_key {
    std::string host;
    uint16_t port = 0;

    bool operator==(const resolve_cache_key& other) const noexcept {
        return host == other.host && port == other.port;
    }
};

struct resolve_cache_key_hash {
    size_t operator()(const resolve_cache_key& key) const noexcept {
        size_t seed = std::hash<std::string>{}(key.host);
        seed ^= static_cast<size_t>(key.port) + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

struct resolve_cache_stats {
    size_t cache_hits = 0;
    size_t cache_misses = 0;
    size_t cache_stores = 0;
    size_t cache_invalidations = 0;
};

struct resolve_cache_entry {
    std::vector<socket_address> addresses;
    std::chrono::steady_clock::time_point expires_at{};
};

class resolve_cache {
public:
    static constexpr size_t shard_count = 16;

    bool try_get(const resolve_cache_key& key, std::vector<socket_address>& out) {
        auto& shard = shard_for(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        prune_expired_locked(shard.entries, std::chrono::steady_clock::now());

        auto it = shard.entries.find(key);
        if (it == shard.entries.end()) {
            return false;
        }

        out = it->second.addresses;
        cache_hits_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void store(const resolve_cache_key& key,
               std::vector<socket_address> addresses,
               std::chrono::seconds ttl) {
        auto& shard = shard_for(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        resolve_cache_entry entry;
        entry.addresses = std::move(addresses);
        entry.expires_at = std::chrono::steady_clock::now() + ttl;
        shard.entries[key] = std::move(entry);
        cache_stores_.fetch_add(1, std::memory_order_relaxed);
    }

    bool invalidate(const std::string_view host, uint16_t port) {
        resolve_cache_key key{std::string(host), port};
        auto& shard = shard_for(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        size_t erased = shard.entries.erase(key);
        if (erased > 0) {
            cache_invalidations_.fetch_add(erased, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    size_t invalidate_host(const std::string_view host) {
        size_t removed = 0;
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            for (auto it = shard.entries.begin(); it != shard.entries.end();) {
                if (it->first.host == host) {
                    it = shard.entries.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
        }
        if (removed > 0) {
            cache_invalidations_.fetch_add(removed, std::memory_order_relaxed);
        }
        return removed;
    }

    void clear() {
        size_t removed = 0;
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            removed += shard.entries.size();
            shard.entries.clear();
        }
        cache_invalidations_.fetch_add(removed, std::memory_order_relaxed);
    }

    resolve_cache_stats stats() const noexcept {
        resolve_cache_stats out;
        out.cache_hits = cache_hits_.load(std::memory_order_relaxed);
        out.cache_misses = cache_misses_.load(std::memory_order_relaxed);
        out.cache_stores = cache_stores_.load(std::memory_order_relaxed);
        out.cache_invalidations = cache_invalidations_.load(std::memory_order_relaxed);
        return out;
    }

    void record_miss() {
        cache_misses_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    struct cache_shard {
        std::mutex mutex;
        std::unordered_map<resolve_cache_key, resolve_cache_entry, resolve_cache_key_hash> entries;
    };

    cache_shard& shard_for(const resolve_cache_key& key) noexcept {
        size_t idx = resolve_cache_key_hash{}(key) % shard_count;
        return shards_[idx];
    }

    static void prune_expired_locked(
        std::unordered_map<resolve_cache_key, resolve_cache_entry, resolve_cache_key_hash>& entries,
        std::chrono::steady_clock::time_point now) {
        for (auto it = entries.begin(); it != entries.end();) {
            if (it->second.expires_at <= now) {
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::array<cache_shard, shard_count> shards_;
    std::atomic<size_t> cache_hits_{0};
    std::atomic<size_t> cache_misses_{0};
    std::atomic<size_t> cache_stores_{0};
    std::atomic<size_t> cache_invalidations_{0};
};

inline resolve_cache& default_resolve_cache() {
    static resolve_cache cache;
    return cache;
}

struct resolve_options {
    bool use_cache = false;
    resolve_cache* cache = nullptr;
    std::chrono::seconds positive_ttl{60};
    std::chrono::seconds negative_ttl{5};
};

inline resolve_options default_cached_resolve_options() {
    resolve_options opts;
    opts.use_cache = true;
    opts.cache = &default_resolve_cache();
    return opts;
}

struct resolve_waiter_state {
    std::vector<socket_address> results;
    int error = 0;
    runtime::scheduler* scheduler = nullptr;
    std::coroutine_handle<> handle;
    size_t saved_affinity = coro::NO_AFFINITY;
    void* handle_address = nullptr;

    void restore_affinity() const noexcept {
        if (!handle_address) {
            return;
        }
        auto* promise = coro::get_promise_base(handle_address);
        if (!promise) {
            return;
        }
        if (saved_affinity == coro::NO_AFFINITY) {
            promise->clear_affinity();
        } else {
            promise->set_affinity(saved_affinity);
        }
    }
};

inline bool try_parse_ipv4_literal(std::string_view host, uint16_t port,
                                   std::vector<socket_address>& out) {
    struct in_addr addr{};
    std::string host_str(host);
    if (inet_pton(AF_INET, host_str.c_str(), &addr) != 1) {
        return false;
    }

    ipv4_address parsed;
    parsed.addr = addr.s_addr;
    parsed.port = port;
    out.push_back(socket_address(parsed));
    return true;
}

inline bool try_parse_ipv6_literal(std::string_view host, uint16_t port,
                                   std::vector<socket_address>& out) {
    std::string ip_str(host);
    uint32_t scope_id = 0;
    size_t scope_pos = ip_str.find('%');
    if (scope_pos != std::string::npos) {
        std::string scope_name = ip_str.substr(scope_pos + 1);
        ip_str = ip_str.substr(0, scope_pos);
        scope_id = if_nametoindex(scope_name.c_str());
    }

    struct in6_addr addr{};
    if (inet_pton(AF_INET6, ip_str.c_str(), &addr) != 1) {
        return false;
    }

    ipv6_address parsed;
    parsed.addr = addr;
    parsed.port = port;
    parsed.scope_id = scope_id;
    out.push_back(socket_address(parsed));
    return true;
}

class resolve_all_awaitable {
public:
    resolve_all_awaitable(std::string_view host, uint16_t port, resolve_options options)
        : host_(host)
        , key_{std::string(host), port}
        , options_(options)
        , state_(std::make_shared<resolve_waiter_state>()) {
        if (host.empty() || host == "::" || host == "0.0.0.0") {
            state_->results.push_back(socket_address(host, port));
            return;
        }

        if (host.find(':') != std::string_view::npos) {
            try_parse_ipv6_literal(host, port, state_->results);
            return;
        }

        try_parse_ipv4_literal(host, port, state_->results);
    }

    bool await_ready() const noexcept {
        if (!state_->results.empty()) {
            return true;
        }

        if (!options_.use_cache) {
            return false;
        }

        resolve_cache* cache = options_.cache ? options_.cache : &default_resolve_cache();
        if (cache->try_get(key_, state_->results)) {
            return true;
        }

        cache->record_miss();
        return false;
    }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> awaiter) {
        state_->handle = awaiter;
        state_->scheduler = runtime::scheduler::current();
        state_->handle_address = awaiter.address();

        if constexpr (std::is_base_of_v<coro::promise_base, Promise>) {
            state_->saved_affinity = awaiter.promise().affinity();
            auto* worker = runtime::worker_thread::current();
            if (worker) {
                awaiter.promise().set_affinity(worker->worker_id());
            }
        }

        auto host = host_;
        auto key = key_;
        auto options = options_;
        auto state = state_;

        std::thread([host = std::move(host), key = std::move(key), options, state]() mutable {
            struct addrinfo hints{};
            struct addrinfo* result = nullptr;
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;

            std::string service = std::to_string(key.port);
            int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
            if (rc == 0 && result) {
                for (auto* current = result; current != nullptr; current = current->ai_next) {
                    if (current->ai_family == AF_INET6) {
                        auto* sa = reinterpret_cast<struct sockaddr_in6*>(current->ai_addr);
                        state->results.push_back(socket_address(ipv6_address(*sa)));
                    } else if (current->ai_family == AF_INET) {
                        auto* sa = reinterpret_cast<struct sockaddr_in*>(current->ai_addr);
                        state->results.push_back(socket_address(ipv4_address(*sa)));
                    }
                }
                freeaddrinfo(result);
            }

            if (state->results.empty()) {
                state->error = (rc == EAI_SYSTEM) ? errno : EHOSTUNREACH;
                if (options.use_cache) {
                    resolve_cache* cache = options.cache ? options.cache : &default_resolve_cache();
                    cache->store(key, {}, options.negative_ttl);
                }
            } else if (options.use_cache) {
                resolve_cache* cache = options.cache ? options.cache : &default_resolve_cache();
                cache->store(key, state->results, options.positive_ttl);
            }

            if (state->scheduler && state->scheduler->is_running()) {
                state->scheduler->spawn(state->handle);
            } else {
                runtime::schedule_handle(state->handle);
            }
        }).detach();

        return true;
    }

    std::vector<socket_address> await_resume() {
        state_->restore_affinity();
        if (state_->results.empty()) {
            errno = state_->error;
        }
        return state_->results;
    }

private:
    std::string host_;
    resolve_cache_key key_;
    resolve_options options_;
    std::shared_ptr<resolve_waiter_state> state_;
};

inline auto resolve_all(std::string_view host,
                        uint16_t port,
                        resolve_options options = {}) {
    return resolve_all_awaitable(host, port, options);
}

inline coro::task<std::optional<socket_address>> resolve_hostname(std::string_view host,
                                                                  uint16_t port,
                                                                  resolve_options options = {}) {
    auto results = co_await resolve_all(host, port, options);
    if (results.empty()) {
        co_return std::nullopt;
    }
    co_return results.front();
}

} // namespace elio::net
