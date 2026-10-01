#pragma once

#include <elio/http/http_common.hpp>
#include <elio/net/resolve_wait.hpp>
#include <elio/tls/tls_context.hpp>

#include <arpa/inet.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace elio::http::detail {

// These tokens identify published policy domains, never object addresses or
// credential contents. Refuse exhaustion rather than reusing an identity.
inline uint64_t new_route_domain() {
    static std::atomic<uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    for (;;) {
        if (value == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("HTTP route domain exhausted");
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            return value;
    }
}

inline std::string normalize_route_host(std::string host) {
    // A zone is case-sensitive even though a DNS name is not.
    const auto zone = host.find('%');
    const auto address = host.substr(0, zone);
    in6_addr v6{};
    char buffer[INET6_ADDRSTRLEN]{};
    if (::inet_pton(AF_INET6, address.c_str(), &v6) == 1 &&
        ::inet_ntop(AF_INET6, &v6, buffer, sizeof(buffer))) {
        return std::string(buffer) + (zone == std::string::npos ? "" : host.substr(zone));
    }
    in_addr v4{};
    if (::inet_pton(AF_INET, host.c_str(), &v4) == 1 &&
        ::inet_ntop(AF_INET, &v4, buffer, sizeof(buffer))) return buffer;
    for (auto& c : host) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return host;
}

struct route_endpoint {
    std::string host;
    uint16_t port = 0;

    static route_endpoint from(std::string host, uint16_t port) {
        return {normalize_route_host(std::move(host)), port};
    }
    std::string authority() const {
        return (host.find(':') == std::string::npos ? host : "[" + host + "]") +
            ":" + std::to_string(port);
    }
    bool operator==(const route_endpoint&) const = default;
};

enum class route_mode { direct, forward_proxy, connect_tunnel };
enum class route_dns_mode { local, proxy }; // Reserved for later proxy DNS routes.
enum class route_protocol { http1, http2 };

struct route_hop_identity {
    route_endpoint endpoint;
    uint64_t tls_domain = 0;
    uint64_t auth_domain = 0;
    route_protocol protocol = route_protocol::http1;
    route_dns_mode dns = route_dns_mode::local;
    bool operator==(const route_hop_identity&) const = default;
};

struct connection_key {
    route_mode mode = route_mode::direct;
    route_endpoint target;
    bool target_secure = false;
    std::vector<route_hop_identity> hops;
    uint64_t connector_domain = 0;
    uint64_t resolution_domain = 0;
    uint64_t origin_tls_domain = 0;
    route_protocol protocol = route_protocol::http1;
    route_dns_mode target_dns = route_dns_mode::local;
    bool operator==(const connection_key&) const = default;
};

#ifdef ELIO_RUNTIME_TEST_HOOKS
inline std::atomic<bool> force_connection_key_collision_for_test{false};
#endif

struct connection_key_hash {
    size_t operator()(const connection_key& key) const noexcept {
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (force_connection_key_collision_for_test.load(std::memory_order_relaxed)) return 0;
#endif
        size_t value = 0;
        auto add = [&value](const auto& field) {
            value ^= std::hash<std::decay_t<decltype(field)>>{}(field) +
                static_cast<size_t>(0x9e3779b9) + (value << 6) + (value >> 2);
        };
        add(key.mode);
        add(key.target.host);
        add(key.target.port);
        add(key.target_secure);
        add(key.hops.size());
        for (const auto& hop : key.hops) {
            add(hop.endpoint.host);
            add(hop.endpoint.port);
            add(hop.tls_domain);
            add(hop.auth_domain);
            add(hop.protocol);
            add(hop.dns);
        }
        add(key.connector_domain);
        add(key.resolution_domain);
        add(key.origin_tls_domain);
        add(key.protocol);
        add(key.target_dns);
        return value;
    }
};

// Published only through shared_ptr<const route_snapshot>. The cache remains
// borrowed under the resolver's existing lifetime contract; DNS/TLS owners are
// retained by every plan. Proxy profiles will add owned hop state here.
struct route_snapshot {
    route_mode mode = route_mode::direct;
    std::vector<route_hop_identity> hops;
    uint64_t connector_domain = new_route_domain();
    uint64_t resolution_domain = new_route_domain();
    uint64_t origin_tls_domain = new_route_domain();
    route_protocol protocol = route_protocol::http1;
    route_dns_mode target_dns = route_dns_mode::local;
    net::resolve_options resolve_options = net::default_cached_resolve_options();
    bool rotate_resolved_addresses = true;
    std::chrono::nanoseconds dns_timeout{0};
    std::shared_ptr<net::resolve_domain> dns_domain;
    std::shared_ptr<tls::tls_context> origin_tls;
};

class route_plan final {
public:
    route_plan(const url& target, std::shared_ptr<const route_snapshot> snapshot)
        : snapshot_(std::move(snapshot)) {
        if (!snapshot_) throw std::invalid_argument("HTTP route requires a snapshot");
        key_.mode = snapshot_->mode;
        key_.target = route_endpoint::from(target.host, target.effective_port());
        key_.target_secure = target.is_secure();
        key_.hops = snapshot_->hops;
        for (auto& hop : key_.hops)
            hop.endpoint.host = normalize_route_host(std::move(hop.endpoint.host));
        key_.connector_domain = snapshot_->connector_domain;
        key_.resolution_domain = snapshot_->resolution_domain;
        key_.origin_tls_domain = target.is_secure() ? snapshot_->origin_tls_domain : 0;
        key_.protocol = snapshot_->protocol;
        key_.target_dns = snapshot_->target_dns;
    }

    const connection_key& key() const noexcept { return key_; }
    const route_snapshot& snapshot() const noexcept { return *snapshot_; }
    const route_endpoint& target() const noexcept { return key_.target; }

private:
    std::shared_ptr<const route_snapshot> snapshot_;
    connection_key key_;
};

} // namespace elio::http::detail
