#pragma once

#include <elio/net/tcp.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/spawn_blocking.hpp>

#include <netdb.h>
#include <net/if.h>
#include <arpa/inet.h>

#include <atomic>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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
    std::optional<int> last_errno;
};

class resolve_cache {
public:
    static constexpr size_t shard_count = 16;

    bool try_get(const resolve_cache_key& key, std::vector<socket_address>& out,
                 std::optional<int>* cached_errno = nullptr) {
        auto& shard = shard_for(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        prune_expired_locked(shard.entries, std::chrono::steady_clock::now());

        auto it = shard.entries.find(key);
        if (it == shard.entries.end()) {
            return false;
        }

        out = it->second.addresses;
        if (cached_errno != nullptr) {
            *cached_errno = it->second.last_errno;
        }
        cache_hits_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void store(const resolve_cache_key& key,
               std::vector<socket_address> addresses,
               std::chrono::seconds ttl,
               std::optional<int> last_err = std::nullopt) {
        auto& shard = shard_for(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        resolve_cache_entry entry;
        entry.addresses = std::move(addresses);
        entry.expires_at = std::chrono::steady_clock::now() + ttl;
        entry.last_errno = last_err;
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
        if (scope_name.empty()) {
            errno = EINVAL;
            return false;
        }

        // Try numeric zone-id first (e.g. "fe80::1%2"); if_nametoindex does
        // not fall through to numeric IDs, so a digit-only zone would
        // otherwise silently become scope_id=0.
        char* end = nullptr;
        unsigned long parsed_zone = std::strtoul(scope_name.c_str(), &end, 10);
        if (end != nullptr && end != scope_name.c_str() && *end == '\0') {
            scope_id = static_cast<uint32_t>(parsed_zone);
        } else {
            scope_id = if_nametoindex(scope_name.c_str());
            if (scope_id == 0) {
                errno = EINVAL;
                return false;
            }
        }
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

namespace detail {

struct dns_lookup_result {
    std::vector<socket_address> addresses;
    int error = 0;
};

struct dns_lookup_request {
    std::string host;
    uint16_t port = 0;
};

struct addrinfo_deleter {
    void operator()(addrinfo* info) const noexcept {
        if (info) {
            freeaddrinfo(info);
        }
    }
};

using addrinfo_owner = std::unique_ptr<addrinfo, addrinfo_deleter>;

struct dns_lookup_operation {
    std::unique_ptr<dns_lookup_request> request;

    explicit dns_lookup_operation(std::unique_ptr<dns_lookup_request> request_in) noexcept
        : request(std::move(request_in)) {}

    dns_lookup_operation(const dns_lookup_operation& other)
        : request(other.request ? std::make_unique<dns_lookup_request>(*other.request) : nullptr) {}

    dns_lookup_operation& operator=(const dns_lookup_operation& other) {
        if (this != &other) {
            request = other.request ? std::make_unique<dns_lookup_request>(*other.request) : nullptr;
        }
        return *this;
    }

    dns_lookup_operation(dns_lookup_operation&&) noexcept = default;
    dns_lookup_operation& operator=(dns_lookup_operation&&) noexcept = default;

    dns_lookup_result operator()() {
        dns_lookup_result result;
        auto request_owner = std::move(request);
        if (!request_owner) {
            result.error = EINVAL;
            return result;
        }

        std::string host = std::move(request_owner->host);
        uint16_t port = request_owner->port;
        request_owner.reset();

        struct addrinfo hints{};
        struct addrinfo* ai_raw_result = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        std::string service = std::to_string(port);
        int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &ai_raw_result);
        addrinfo_owner ai_result(ai_raw_result);
        if (rc == 0 && ai_result) {
            for (auto* current = ai_result.get(); current != nullptr; current = current->ai_next) {
                if (current->ai_family == AF_INET6) {
                    auto* sa = reinterpret_cast<struct sockaddr_in6*>(current->ai_addr);
                    result.addresses.push_back(socket_address(ipv6_address(*sa)));
                } else if (current->ai_family == AF_INET) {
                    auto* sa = reinterpret_cast<struct sockaddr_in*>(current->ai_addr);
                    result.addresses.push_back(socket_address(ipv4_address(*sa)));
                }
            }
        }

        if (result.addresses.empty()) {
            result.error = (rc == EAI_SYSTEM) ? errno : EHOSTUNREACH;
        }
        return result;
    }
};

}  // namespace detail

inline coro::task<std::vector<socket_address>> resolve_all(
    std::string_view host,
    uint16_t port,
    resolve_options options = {}) {

    std::vector<socket_address> results;

    // Handle empty host or wildcard addresses
    if (host.empty() || host == "::" || host == "0.0.0.0") {
        results.push_back(socket_address(host, port));
        co_return results;
    }

    // Try parsing as IPv6 literal
    if (host.find(':') != std::string_view::npos) {
        if (try_parse_ipv6_literal(host, port, results)) {
            co_return results;
        }
    }

    // Try parsing as IPv4 literal
    if (try_parse_ipv4_literal(host, port, results)) {
        co_return results;
    }

    // Check cache if enabled
    resolve_cache_key key{std::string(host), port};
    if (options.use_cache) {
        resolve_cache* cache = options.cache ? options.cache : &default_resolve_cache();
        std::optional<int> cached_err;
        if (cache->try_get(key, results, &cached_err)) {
            // Negative-cache hit: surface the errno captured at the original
            // failure so callers can distinguish failure modes from arbitrary
            // stale errno on this thread.
            if (results.empty()) {
                errno = cached_err.value_or(EHOSTUNREACH);
            }
            co_return results;
        }
        cache->record_miss();
    }

    // Perform blocking DNS resolution via spawn_blocking. The operation owns
    // the request without carrying string captures in the coroutine frame.
    auto request = std::make_unique<detail::dns_lookup_request>(
        detail::dns_lookup_request{std::string(host), port});
    auto dns_result = co_await elio::spawn_blocking(
        detail::dns_lookup_operation(std::move(request)));

    // Update cache based on result
    if (options.use_cache) {
        resolve_cache* cache = options.cache ? options.cache : &default_resolve_cache();
        if (dns_result.addresses.empty()) {
            cache->store(key, {}, options.negative_ttl, dns_result.error);
        } else {
            cache->store(key, dns_result.addresses, options.positive_ttl);
        }
    }

    // Set errno on failure
    if (dns_result.addresses.empty()) {
        errno = dns_result.error;
    }

    co_return dns_result.addresses;
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
