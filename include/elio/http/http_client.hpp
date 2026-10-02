#pragma once

#include <elio/http/http_common.hpp>
#include <elio/http/http_parser.hpp>
#include <elio/http/http_response_reader.hpp>
#include <elio/http/http_response_body_reader.hpp>
#include <elio/http/http_message.hpp>
#include <elio/http/client_base.hpp>
#include <elio/http/detail/route_plan.hpp>
#include <elio/http/detail/bounded_pool.hpp>
#include <elio/net/stream.hpp>
#include <elio/io/io_context.hpp>
#include <elio/coro/task.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/sync/event.hpp>
#include <elio/time/timer.hpp>
#include <elio/log/macros.hpp>

#include <sys/socket.h>

#include <atomic>
#include <string>
#include <string_view>
#include <memory>
#include <array>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <concepts>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace elio::http {

#ifdef ELIO_RUNTIME_TEST_HOOKS
namespace detail {
using route_connect_hook = coro::task<client_result<net::stream>> (*)(
    const route_plan&, std::chrono::nanoseconds, coro::cancel_token);
inline std::atomic<route_connect_hook> route_connect_for_test{nullptr};
using route_deadline_hook = void (*)(std::optional<std::chrono::steady_clock::time_point>);
inline std::atomic<route_deadline_hook> route_deadline_for_test{nullptr};
using acquisition_now_hook = std::chrono::steady_clock::time_point (*)() noexcept;
inline std::atomic<acquisition_now_hook> acquisition_now_for_test{nullptr};
inline std::atomic<void(*)()> idle_taken_for_test{nullptr};
using lease_disposition_hook = void (*)(int, bool);
inline std::atomic<lease_disposition_hook> lease_disposition_for_test{nullptr};
using lease_return_hook = void (*)();
inline std::atomic<lease_return_hook> lease_before_return_for_test{nullptr};
inline std::atomic<lease_return_hook> transport_clear_visible_for_test{nullptr};
// Expire only the Expect clock after final headers, allowing a regression to
// hold the final body behind a barrier without relying on timer scheduling.
inline std::atomic<bool> expire_expect_after_headers_for_test{false};
inline std::atomic<bool> final_headers_seen_for_test{false};
// Inject a terminal write result without depending on TCP close timing.
using request_write_hook = io::io_result (*)(std::string_view);
inline std::atomic<request_write_hook> request_write_result_for_test{nullptr};
inline std::atomic<client_stage> response_read_stage_for_test{client_stage::headers};
using response_headers_hook = void (*)(uint16_t);
inline std::atomic<response_headers_hook> response_headers_for_test{nullptr};
using response_deadline_hook = void (*)(std::chrono::steady_clock::time_point);
inline std::atomic<response_deadline_hook> response_deadline_for_test{nullptr};
using response_watchdog_wait_hook = coro::task<coro::cancel_result> (*)(
    std::chrono::nanoseconds, coro::cancel_token, client_stage);
inline std::atomic<response_watchdog_wait_hook> response_watchdog_wait_for_test{nullptr};
inline std::atomic<void(*)(int)> deferred_upload_for_test{nullptr};
using deferred_upload_result_hook = coro::task<void> (*)(const std::optional<client_error>&);
inline std::atomic<deferred_upload_result_hook> deferred_upload_result_for_test{nullptr};
inline std::atomic<void(*)(io::io_result)> response_transport_result_for_test{nullptr};
} // namespace detail
#endif

namespace detail {
inline std::chrono::steady_clock::time_point acquisition_now() noexcept {
#ifdef ELIO_RUNTIME_TEST_HOOKS
    if (auto hook = acquisition_now_for_test.load(std::memory_order_acquire)) return hook();
#endif
    return std::chrono::steady_clock::now();
}
} // namespace detail

/// HTTP client configuration
struct client_config : base_client_config {
    size_t max_redirects = 5;                     ///< Max redirects to follow
    bool follow_redirects = true;                 ///< Auto-follow redirects
    size_t max_connections_per_host = 6;          ///< Max connections per host
    std::chrono::seconds pool_idle_timeout{60};   ///< Idle connection timeout
    std::optional<pool_limits> limits;             ///< Opt-in finite Transport admission
    std::chrono::nanoseconds acquisition_timeout{0}; ///< Queue + DNS + TCP + TLS; <=0 disables
    /// Buffered APIs cap the accumulated final body and, separately, the
    /// cumulative informational wire bytes. with_response uses its own options
    /// for these limits and never allocates a body-sized receive buffer.
    size_t max_response_size = 16 * 1024 * 1024;  ///< Max response body size (16 MiB)
    /// Deadline for waiting on an interim 100 Continue when a request uses
    /// request::set_expect_continue(). On expiry the body is sent anyway
    /// (RFC 9110 §10.1.1 fallback). A value <= 0 skips the wait: the body
    /// is sent immediately after the headers. The wait is additionally
    /// bounded by the absolute response deadline (read_timeout), whichever
    /// expires first; response-deadline expiry fails the request with
    /// ETIMEDOUT instead of triggering the fallback.
    std::chrono::milliseconds expect_continue_timeout{1000};

    client_config() {
        user_agent = "elio-http/1.0";
    }
};

/// Construction-only TLS policy builder for http::transport.
///
/// The builder exposes TLS policy mutators before publication without handing
/// callbacks a mutable reference to the transport's published tls_context.
/// Diagnostics are limited to copied values so callbacks cannot retain access
/// to mutable OpenSSL-owned state after publication.
class transport_tls_config {
public:
    explicit transport_tls_config(bool verify_certificate = true)
        : ctx_(tls::tls_mode::client) {
        init_client_tls_context(ctx_, verify_certificate);
    }

    transport_tls_config(const transport_tls_config&) = delete;
    transport_tls_config& operator=(const transport_tls_config&) = delete;
    transport_tls_config(transport_tls_config&&) = delete;
    transport_tls_config& operator=(transport_tls_config&&) = delete;

    bool load_certificate(std::string_view cert_file) {
        return ctx_.load_certificate(cert_file);
    }

    bool load_private_key(std::string_view key_file, std::string_view password = {}) {
        return ctx_.load_private_key(key_file, password);
    }

    bool load_verify_locations(std::string_view ca_file = {},
                               std::string_view ca_path = {}) {
        return ctx_.load_verify_locations(ca_file, ca_path);
    }

    bool use_default_verify_paths() { return ctx_.use_default_verify_paths(); }
    void set_verify_mode(tls::verify_mode mode) { ctx_.set_verify_mode(mode); }
    bool set_alpn_protocols(std::string_view protocols) {
        return ctx_.set_alpn_protocols(protocols);
    }
    bool set_ciphers(std::string_view ciphers) { return ctx_.set_ciphers(ciphers); }
    bool set_ciphersuites(std::string_view ciphersuites) {
        return ctx_.set_ciphersuites(ciphersuites);
    }

    tls::tls_mode mode() const noexcept { return ctx_.mode(); }
    long verify_mode() const noexcept { return ctx_.verify_mode(); }

private:
    friend class transport;

    tls::tls_context release_context() && noexcept { return std::move(ctx_); }

    tls::tls_context ctx_;
};

struct transport_tls_diagnostics {
    tls::tls_mode mode = tls::tls_mode::client;
    long verify_mode = 0;
};

/// Immutable connection-establishment and pooling configuration owned by
/// http::transport. Request policy stays on http::client.
struct transport_config {
    bool verify_certificate = true;               ///< Verify TLS certificates
    net::resolve_options resolve_options = net::default_cached_resolve_options();
    bool rotate_resolved_addresses = true;        ///< Rotate through DNS results
    std::chrono::nanoseconds dns_timeout{0};      ///< DNS observer budget
    std::shared_ptr<net::resolve_domain> dns_domain; ///< DNS admission domain
    size_t max_connections_per_host = 6;          ///< Max retained idle connections per host
    std::chrono::seconds pool_idle_timeout{60};   ///< Idle connection timeout
    std::optional<pool_limits> limits;             ///< Absent preserves legacy admission
    std::chrono::nanoseconds acquisition_timeout{0}; ///< One absolute acquisition budget
    /// Optional construction-time TLS customization. It runs after Elio's
    /// default client TLS initialization on a builder that is moved into the
    /// transport only after this callback returns.
    std::function<void(transport_tls_config&)> configure_tls;

    transport_config() = default;

    explicit transport_config(const client_config& config)
        : verify_certificate(config.verify_certificate)
        , resolve_options(config.resolve_options)
        , rotate_resolved_addresses(config.rotate_resolved_addresses)
        , dns_timeout(config.dns_timeout)
        , dns_domain(config.dns_domain)
        , max_connections_per_host(config.max_connections_per_host)
        , pool_idle_timeout(config.pool_idle_timeout)
        , limits(config.limits)
        , acquisition_timeout(config.acquisition_timeout) {}
};

/// Connection wrapper using unified net::stream
using connection = net::stream;

/// Connection pool for HTTP keep-alive. Legacy host/port/scheme adapters require
/// the original return authority and one stable caller-owned TLS/resolver policy
/// per pool. For a policy change, settle old operations and dispose of checked-out
/// connections before clearing, or keep a separate old pool/context alive for
/// old operations/returns. Shared transports use the private plan-based path.
class connection_pool {
public:
    static constexpr size_t shard_count = 16;

    /// Per-acquisition snapshot; an explicit null domain selects the shared
    /// resolver default instead of retaining a constructor-time custom domain.
    struct dns_options {
        dns_options() = default;

        dns_options(std::chrono::nanoseconds timeout_value,
                    std::shared_ptr<net::resolve_domain> domain_value = {}) noexcept
            : timeout(timeout_value), domain(std::move(domain_value)) {}

        std::chrono::nanoseconds timeout{0};
        std::shared_ptr<net::resolve_domain> domain{};
    };

    explicit connection_pool(transport_config config = {})
        : config_(config) {}

    explicit connection_pool(const client_config& config)
        : connection_pool(transport_config(config)) {}

    /// Get or create a connection to host
    coro::task<client_result<connection>> acquire_result(const std::string& host,
                                                   uint16_t port,
                                                   bool secure,
                                                   tls::tls_context* tls_ctx = nullptr,
                                                   std::chrono::nanoseconds connect_timeout =
                                                       std::chrono::nanoseconds::zero(),
                                                   coro::cancel_token token = {},
                                                   std::optional<dns_options> dns = std::nullopt) {
        auto conn = take_idle(make_legacy_key(host, port, secure));
        if (conn.has_value()) {
            co_return std::move(*conn);
        }
        if (token.is_cancelled()) {
            co_return detail::make_client_error(ECANCELED, client_stage::acquire);
        }

        // Create new connection using client_connect utility
        auto result = co_await client_connect_result(
            host,
            port,
            secure,
            tls_ctx,
            config_.resolve_options,
            config_.rotate_resolved_addresses,
            connect_timeout,
            std::move(token),
            dns ? dns->timeout : config_.dns_timeout,
            dns ? dns->domain : config_.dns_domain);
        co_return std::move(result);
    }

    coro::task<std::optional<connection>> acquire(const std::string& host,
            uint16_t port, bool secure, tls::tls_context* tls_ctx = nullptr,
            std::chrono::nanoseconds connect_timeout = std::chrono::nanoseconds::zero(),
            coro::cancel_token token = {},
            std::optional<dns_options> dns = std::nullopt) {
        auto result = co_await acquire_result(host, port, secure, tls_ctx,
                                             connect_timeout, std::move(token), std::move(dns));
        if (const auto* error = std::get_if<client_error>(&result)) {
            errno = error->code.value();
            co_return std::nullopt;
        }
        co_return std::move(std::get<connection>(result));
    }

    /// Return a connection to the pool
    void release(const std::string& host, uint16_t port, bool secure, connection conn) {
        retain_key(make_legacy_key(host, port, secure), conn);
    }

    /// Clear all pooled connections
    void clear() {
        auto retired = detach_idle();
    }

#ifdef ELIO_RUNTIME_TEST_HOOKS
    coro::task<client_result<connection>> acquire_plan_for_test(
            detail::route_plan plan, coro::cancel_token token = {}) {
        return acquire_plan(std::move(plan), {}, std::move(token));
    }
    void release_plan_for_test(const detail::route_plan& plan, connection conn) {
        retain_key(plan.key(), conn);
    }
#endif

private:
    friend class transport;

    using pool_map = std::unordered_map<detail::connection_key, std::deque<connection>,
                                       detail::connection_key_hash>;
    using retired_pools = std::array<pool_map, shard_count>;

    retired_pools detach_idle() {
        retired_pools retired;
        for (size_t i = 0; i < shard_count; ++i) {
            std::lock_guard lock(shards_[i].mutex);
            retired[i].swap(shards_[i].pools);
        }
        return retired;
    }

    detail::connection_key make_legacy_key(const std::string& host, uint16_t port,
                                           bool secure) const {
        detail::connection_key key;
        key.target = detail::route_endpoint::from(host, port);
        key.target_secure = secure;
        key.connector_domain = legacy_domain_;
        key.resolution_domain = legacy_domain_;
        return key;
    }

    std::optional<connection> take_idle(const detail::connection_key& key) {
        std::vector<connection> retired;
        std::optional<connection> conn;
        connection candidate;
        auto& shard = shard_for(key);
        {
            std::lock_guard lock(shard.mutex);
            auto it = shard.pools.find(key);
            if (it == shard.pools.end()) return std::nullopt;
            while (!it->second.empty()) {
                candidate = std::move(it->second.front());
                it->second.pop_front();
                if (std::chrono::steady_clock::now() - candidate.last_use() <
                        config_.pool_idle_timeout) {
                    candidate.touch();
                    conn = std::move(candidate);
                    break;
                }
                retired.push_back(std::move(candidate));
            }
            if (it->second.empty()) shard.pools.erase(it);
        }
        return conn;
    }

    bool retain_key(const detail::connection_key& key, connection& conn) {
        if (config_.max_connections_per_host == 0) return false;
        auto& shard = shard_for(key);
        std::lock_guard lock(shard.mutex);
        auto& pool = shard.pools[key];
        if (pool.size() < config_.max_connections_per_host) {
            conn.touch();
            pool.push_back(std::move(conn));
            return true;
        }
        return false;
    }

    coro::task<client_result<connection>> acquire_plan(detail::route_plan plan,
            std::chrono::nanoseconds connect_timeout, coro::cancel_token token,
            std::optional<std::chrono::steady_clock::time_point> deadline = {}) {
        if (token.is_cancelled())
            co_return detail::make_client_error(ECANCELED, client_stage::acquire);
        if (deadline && *deadline <= detail::acquisition_now())
            co_return detail::make_client_error(ETIMEDOUT, client_stage::acquire);
        if (auto conn = take_idle(plan.key())) {
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (auto hook = detail::idle_taken_for_test.load(std::memory_order_acquire)) hook();
#endif
            if (token.is_cancelled())
                co_return detail::make_client_error(ECANCELED, client_stage::acquire);
            if (deadline && *deadline <= detail::acquisition_now())
                co_return detail::make_client_error(ETIMEDOUT, client_stage::acquire);
            co_return std::move(*conn);
        }
        co_return co_await connect_plan(std::move(plan), connect_timeout, std::move(token), deadline);
    }

    coro::task<client_result<connection>> connect_plan(detail::route_plan plan,
            std::chrono::nanoseconds connect_timeout, coro::cancel_token token,
            std::optional<std::chrono::steady_clock::time_point> deadline = {}) {
        if (token.is_cancelled())
            co_return detail::make_client_error(ECANCELED, client_stage::acquire);
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = detail::route_deadline_for_test.load(std::memory_order_acquire)) hook(deadline);
        if (auto hook = detail::route_connect_for_test.load(std::memory_order_acquire))
            co_return co_await hook(plan, connect_timeout, std::move(token));
#endif
        // Identity is modeled now; proxy connectors are delivered separately.
        if (plan.key().mode != detail::route_mode::direct || !plan.key().hops.empty() ||
            plan.key().protocol != detail::route_protocol::http1 ||
            plan.key().target_dns != detail::route_dns_mode::local)
            co_return detail::make_client_error(ENOTSUP, client_stage::acquire);
        const auto& snapshot = plan.snapshot();
        co_return co_await detail::client_connect_result_impl(plan.target().host, plan.target().port,
            plan.key().target_secure, snapshot.origin_tls.get(), snapshot.resolve_options,
            snapshot.rotate_resolved_addresses, connect_timeout, std::move(token),
            snapshot.dns_timeout, snapshot.dns_domain, deadline, true);
    }

    struct pool_shard {
        std::mutex mutex;
        pool_map pools;
    };

    pool_shard& shard_for(const detail::connection_key& key) noexcept {
        return shards_[detail::connection_key_hash{}(key) % shard_count];
    }

    transport_config config_;
    const uint64_t legacy_domain_ = detail::new_route_domain();
    std::array<pool_shard, shard_count> shards_;
};

/// Shared owner for HTTP/1 connection establishment, TLS security context and
/// idle pooling. Separate client instances may share a transport while keeping
/// independent redirect/body/user-agent request policy.
class transport {
private:
    friend class client;
    struct state {
        explicit state(transport_config value)
            : config(std::move(value)), snapshot(make_route_snapshot(config)), pool(config),
              admission(config.limits ? std::make_unique<detail::bounded_pool<connection>>(
                  *config.limits, config.pool_idle_timeout) : nullptr) {
            settled.set();
        }

        void finish_operation() noexcept {
            bool notify = false;
            {
                std::lock_guard lock(mutex);
                if (active_operations == 0) return;
                notify = --active_operations == 0;
            }
            if (notify) settled.set();
        }

        // The snapshot outlives idle stream destruction, including TLS state.
        const transport_config config;
        const std::shared_ptr<const detail::route_snapshot> snapshot;
        connection_pool pool;
        std::unique_ptr<detail::bounded_pool<connection>> admission;
        mutable std::mutex mutex;
        sync::event settled;
        size_t active_operations = 0;
        uint64_t generation = detail::new_route_domain();
        bool closing = false;
    };

    class operation_lease {
    public:
        operation_lease() noexcept = default;
        operation_lease(operation_lease&& other) noexcept
            : owner_(std::move(other.owner_)), generation_(other.generation_) {}
        operation_lease& operator=(operation_lease&& other) noexcept {
            if (this != &other) {
                reset();
                owner_ = std::move(other.owner_);
                generation_ = other.generation_;
            }
            return *this;
        }
        operation_lease(const operation_lease&) = delete;
        operation_lease& operator=(const operation_lease&) = delete;
        ~operation_lease() { reset(); }
        operation_lease(std::shared_ptr<state> owner, uint64_t generation) noexcept
            : owner_(std::move(owner)), generation_(generation) {}
        void reset() noexcept {
            if (auto owner = std::move(owner_)) owner->finish_operation();
        }
        const std::shared_ptr<state>& owner() const noexcept { return owner_; }
        uint64_t generation() const noexcept { return generation_; }

    private:
        std::shared_ptr<state> owner_;
        uint64_t generation_ = 0;
    };

    class connection_lease {
    public:
        connection_lease(connection_lease&& other) noexcept
            : operation_(std::move(other.operation_)), plan_(std::move(other.plan_)),
              conn_(std::move(other.conn_)), capacity_(std::move(other.capacity_)), disposition_(
                  std::exchange(other.disposition_, disposition::empty)) {}
        connection_lease& operator=(connection_lease&& other) noexcept {
            if (this != &other) {
                retire();
                operation_ = std::move(other.operation_);
                plan_ = std::move(other.plan_);
                conn_ = std::move(other.conn_);
                capacity_ = std::move(other.capacity_);
                disposition_ = std::exchange(other.disposition_, disposition::empty);
            }
            return *this;
        }
        connection_lease(const connection_lease&) = delete;
        connection_lease& operator=(const connection_lease&) = delete;
        ~connection_lease() { retire(); }

        connection& stream() noexcept { return conn_; }
        const detail::route_plan& plan() const noexcept { return plan_; }
        void retire() noexcept {
            if (disposition_ != disposition::active) return;
            const int fd = conn_.fd();
            disposition_ = disposition::retired;
            detail::abort_stream_io(conn_);
            conn_.disconnect();
            capacity_.reset();
            observe_disposition(fd, false);
            operation_.reset();
        }

    private:
        friend class transport;
        friend class client;
        enum class disposition { empty, active, returned, retired };

        connection_lease(operation_lease operation, detail::route_plan plan,
                         connection conn,
                         detail::bounded_pool<connection>::permit capacity = {}) noexcept
            : operation_(std::move(operation)), plan_(std::move(plan)),
              conn_(std::move(conn)), capacity_(std::move(capacity)) {}

        // Only the exchange owner may call this after validating completion.
        // No public boolean can bypass the response framing and I/O settlement.
        void return_reusable() {
            if (disposition_ != disposition::active) return;
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (auto hook = detail::lease_before_return_for_test.load()) hook();
#endif
            const auto owner = operation_.owner();
            bool retained = false;
            const int fd = conn_.fd();
            std::optional<detail::bounded_pool<connection>::change> admission_change;
            {
                // clear/shutdown publish their boundary under this lock, so a
                // late insertion cannot escape it. Stream cleanup runs outside.
                std::lock_guard lock(owner->mutex);
                if (!owner->closing && operation_.generation() == owner->generation) {
                    if (owner->admission) {
                        admission_change.emplace(owner->admission->retain(capacity_, conn_));
                        retained = admission_change->retained;
                    } else retained = owner->pool.retain_key(plan_.key(), conn_);
                }
            }
            if (!retained) {
                retire();
                return;
            }
            disposition_ = disposition::returned;
            conn_.disconnect();
            observe_disposition(fd, true);
            operation_.reset();
        }

        static void observe_disposition(int fd, bool returned) noexcept {
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (auto hook = detail::lease_disposition_for_test.load()) hook(fd, returned);
#else
            (void)fd;
            (void)returned;
#endif
        }

        // Retirement closes the stream before settlement releases the owner.
        operation_lease operation_;
        detail::route_plan plan_;
        connection conn_;
        detail::bounded_pool<connection>::permit capacity_;
        disposition disposition_ = disposition::active;
    };

public:
    explicit transport(transport_config config = {})
        : state_(std::make_shared<state>(std::move(config))) {}
    explicit transport(const client_config& config)
        : transport(transport_config(config)) {}

    transport(const transport&) = delete;
    transport& operator=(const transport&) = delete;
    transport(transport&&) = delete;
    transport& operator=(transport&&) = delete;

    /// Drop idle entries and reject later returns of pre-clear leases. Existing
    /// I/O continues; use shutdown() to stop new acquisitions and await settlement.
    void clear() {
        connection_pool::retired_pools retired;
        std::optional<detail::bounded_pool<connection>::change> admission_change;
        {
            std::lock_guard lock(state_->mutex);
            state_->generation = detail::new_route_domain();
            retired = state_->pool.detach_idle();
            if (state_->admission) admission_change.emplace(state_->admission->clear());
        }
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = detail::transport_clear_visible_for_test.load()) hook();
#endif
    }

    /// Stop acquisitions and await client-managed dialing/exchange settlement.
    /// Cancellation only stops this wait; it never destroys active I/O frames.
    coro::task<coro::cancel_result> shutdown(coro::cancel_token token = {}) {
        const auto owner = state_;
        {
            connection_pool::retired_pools retired;
            std::optional<detail::bounded_pool<connection>::change> admission_change;
            {
                std::lock_guard lock(owner->mutex);
                owner->closing = true;
                retired = owner->pool.detach_idle();
                if (owner->admission) admission_change.emplace(owner->admission->clear(true));
            }
        }
        for (;;) {
            {
                std::lock_guard lock(owner->mutex);
                if (owner->active_operations == 0) co_return coro::cancel_result::completed;
                // A prior zero-count signal can race a later operation's reset.
                // Reset under the count lock; current settlement signals outside.
                owner->settled.reset();
            }
            if (co_await owner->settled.wait(token) == coro::cancel_result::cancelled)
                co_return coro::cancel_result::cancelled;
        }
    }

    transport_tls_diagnostics tls_diagnostics() const noexcept {
        return {.mode = state_->snapshot->origin_tls->mode(),
                .verify_mode = state_->snapshot->origin_tls->verify_mode()};
    }
    const transport_config& config() const noexcept { return state_->config; }
    bool is_shutdown() const noexcept {
        std::lock_guard lock(state_->mutex);
        return state_->closing;
    }

#ifdef ELIO_RUNTIME_TEST_HOOKS
    bool start_operation_for_test() {
        std::lock_guard lock(state_->mutex);
        if (state_->closing) return false;
        if (state_->active_operations++ == 0) state_->settled.reset();
        return true;
    }
    void finish_operation_for_test() noexcept { state_->finish_operation(); }
    size_t active_operations_for_test() const {
        std::lock_guard lock(state_->mutex);
        return state_->active_operations;
    }
    void signal_settled_for_test() { state_->settled.set(); }
    detail::route_plan route_plan_for_test(const url& target) const {
        return detail::route_plan(target, state_->snapshot);
    }
    std::weak_ptr<void> state_owner_for_test() const { return state_; }
    using connection_lease_for_test = connection_lease;
    auto admission_counters_for_test() const {
        return state_->admission->counters_for_test();
    }
    coro::task<client_result<connection_lease>> acquire_lease_for_test(
            url target, coro::cancel_token token = {}) {
        return acquire_leased_result(target, {}, std::move(token));
    }
    static void return_lease_for_test(connection_lease& lease) { lease.return_reusable(); }
    coro::task<client_result<connection>> acquire_result_for_test(
            const url& target, std::chrono::nanoseconds connect_timeout,
            coro::cancel_token token = {}) {
        if (state_->admission)
            co_return detail::make_client_error(ENOTSUP, client_stage::acquire);
        auto result = co_await acquire_leased_result(target, connect_timeout, std::move(token));
        if (const auto* error = std::get_if<client_error>(&result)) co_return *error;
        auto lease = std::move(std::get<connection_lease>(result));
        // Historical test adapter only; production exchanges cannot detach I/O.
        lease.disposition_ = connection_lease::disposition::empty;
        co_return std::move(lease.stream());
    }
#endif

private:
    static tls::tls_context make_tls_context(const transport_config& config) {
        transport_tls_config tls_config(config.verify_certificate);
        if (config.configure_tls) config.configure_tls(tls_config);
        return std::move(tls_config).release_context();
    }
    static std::shared_ptr<const detail::route_snapshot> make_route_snapshot(
            const transport_config& config) {
        detail::route_snapshot snapshot;
        snapshot.resolve_options = config.resolve_options;
        snapshot.rotate_resolved_addresses = config.rotate_resolved_addresses;
        snapshot.dns_timeout = config.dns_timeout;
        snapshot.dns_domain = config.dns_domain;
        snapshot.origin_tls = std::make_shared<tls::tls_context>(make_tls_context(config));
        return std::make_shared<const detail::route_snapshot>(std::move(snapshot));
    }
    static std::optional<operation_lease> try_acquire_lease(const std::shared_ptr<state>& owner) {
        std::lock_guard lock(owner->mutex);
        if (owner->closing) return std::nullopt;
        if (owner->active_operations++ == 0) owner->settled.reset();
        return operation_lease(owner, owner->generation);
    }
    coro::task<client_result<connection_lease>> acquire_leased_result(
            const url& target, std::chrono::nanoseconds timeout, coro::cancel_token token) {
        return acquire_owned_result(state_, detail::route_plan(target, state_->snapshot),
                                    timeout, std::move(token));
    }
    static coro::task<client_result<connection_lease>> acquire_owned_result(
            std::shared_ptr<state> owner, detail::route_plan plan,
            std::chrono::nanoseconds connect_timeout,
            coro::cancel_token token) {
        auto lease = try_acquire_lease(owner);
        if (!lease) co_return detail::make_client_error(ESHUTDOWN, client_stage::acquire);
        std::optional<std::chrono::steady_clock::time_point> deadline;
        if (owner->config.acquisition_timeout.count() > 0) {
            const auto now = detail::acquisition_now();
            const auto remaining = std::chrono::steady_clock::time_point::max() - now;
            deadline = owner->config.acquisition_timeout >= remaining
                ? std::chrono::steady_clock::time_point::max()
                : now + owner->config.acquisition_timeout;
        }
        detail::bounded_pool<connection>::permit capacity;
        if (owner->admission) {
            auto admitted = co_await owner->admission->acquire(plan.key(), deadline, token);
            if (const auto* error = std::get_if<client_error>(&admitted)) co_return *error;
            auto granted = std::move(std::get<detail::bounded_pool<connection>::grant>(admitted));
            capacity = std::move(granted.capacity);
            if (token.is_cancelled())
                co_return detail::make_client_error(ECANCELED, client_stage::acquire);
            if (deadline && *deadline <= detail::acquisition_now())
                co_return detail::make_client_error(ETIMEDOUT, client_stage::acquire);
            if (granted.idle)
                co_return connection_lease(std::move(*lease), std::move(plan),
                    std::move(*granted.idle), std::move(capacity));
        }
        auto acquisition = owner->admission
            ? owner->pool.connect_plan(plan, connect_timeout, token, deadline)
            : owner->pool.acquire_plan(plan, connect_timeout, token, deadline);
        auto conn_result = co_await std::move(acquisition);
        capacity.dial_complete();
        if (const auto* error = std::get_if<client_error>(&conn_result)) co_return *error;
        co_return connection_lease(std::move(*lease), std::move(plan),
            std::move(std::get<connection>(conn_result)), std::move(capacity));
    }

    const std::shared_ptr<state> state_;
};

/// HTTP client
class client {
public:
    /// Create client with default configuration
    client() : client(client_config{}) {}

    /// Create client with configuration
    explicit client(client_config config)
        : config_(std::move(config))
        , transport_(std::make_shared<transport>(config_)) {}

    /// Create client with an explicitly shared transport and independent
    /// request policy. The transport owns connection pooling and TLS security
    /// snapshots; this client owns redirects, headers and response limits.
    explicit client(std::shared_ptr<transport> shared_transport,
                    client_config config = {})
        : config_(std::move(config))
        , transport_(std::move(shared_transport)) {
        if (!transport_) throw std::invalid_argument("http::client requires a transport");
    }

    client(const client&) = delete;
    client& operator=(const client&) = delete;
    client(client&&) noexcept = default;
    client& operator=(client&&) noexcept = default;

    /// Perform HTTP GET request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> get(std::string_view url_str) {
        return request_url(method::GET, url_str, "", "", coro::cancel_token{});
    }

    /// Perform HTTP GET request with cancellation support
    coro::task<std::optional<response>> get(std::string_view url_str, coro::cancel_token token) {
        return request_url(method::GET, url_str, "", "", std::move(token));
    }

    /// Perform HTTP POST request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> post(std::string_view url_str,
                                                   std::string_view body,
                                                   std::string_view content_type = mime::application_form_urlencoded) {
        return request_url(method::POST, url_str, body, content_type, coro::cancel_token{});
    }

    /// Perform HTTP POST request with cancellation support
    coro::task<std::optional<response>> post(std::string_view url_str,
                                                   std::string_view body,
                                                   coro::cancel_token token,
                                                   std::string_view content_type = mime::application_form_urlencoded) {
        return request_url(method::POST, url_str, body, content_type, std::move(token));
    }

    /// Perform HTTP PUT request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> put(std::string_view url_str,
                                                  std::string_view body,
                                                  std::string_view content_type = mime::application_json) {
        return request_url(method::PUT, url_str, body, content_type, coro::cancel_token{});
    }

    /// Perform HTTP PUT request with cancellation support
    coro::task<std::optional<response>> put(std::string_view url_str,
                                                  std::string_view body,
                                                  coro::cancel_token token,
                                                  std::string_view content_type = mime::application_json) {
        return request_url(method::PUT, url_str, body, content_type, std::move(token));
    }

    /// Perform HTTP DELETE request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> del(std::string_view url_str) {
        return request_url(method::DELETE_, url_str, "", "", coro::cancel_token{});
    }

    /// Perform HTTP DELETE request with cancellation support
    coro::task<std::optional<response>> del(std::string_view url_str, coro::cancel_token token) {
        return request_url(method::DELETE_, url_str, "", "", std::move(token));
    }

    /// Perform HTTP PATCH request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> patch(std::string_view url_str,
                                                    std::string_view body,
                                                    std::string_view content_type = mime::application_json) {
        return request_url(method::PATCH, url_str, body, content_type, coro::cancel_token{});
    }

    /// Perform HTTP PATCH request with cancellation support
    coro::task<std::optional<response>> patch(std::string_view url_str,
                                                    std::string_view body,
                                                    coro::cancel_token token,
                                                    std::string_view content_type = mime::application_json) {
        return request_url(method::PATCH, url_str, body, content_type, std::move(token));
    }

    /// Perform HTTP HEAD request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> head(std::string_view url_str) {
        return request_url(method::HEAD, url_str, "", "", coro::cancel_token{});
    }

    /// Perform HTTP HEAD request with cancellation support
    coro::task<std::optional<response>> head(std::string_view url_str, coro::cancel_token token) {
        return request_url(method::HEAD, url_str, "", "", std::move(token));
    }

    /// Send a custom request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> send(request& req, const url& target) {
        co_return co_await send(req, target, coro::cancel_token{});
    }

    /// Send a custom request with cancellation support
    coro::task<std::optional<response>> send(request& req, const url& target, coro::cancel_token token) {
        co_return optional_response(co_await send_result(req, target, std::move(token)));
    }

    /// Owned operational errors; allocation/setup/programming exceptions may
    /// propagate. Keep this client, req, and target alive through awaited return.
    coro::task<client_result<response>> send_result(
            request& req, const url& target, coro::cancel_token token = {}) {
        if (!detail::is_supported_http_url_scheme(target.scheme)) {
            co_return detail::make_client_error(EINVAL, client_stage::target);
        }
        co_return co_await send_request(req, target, 0, std::move(token));
    }

    coro::task<client_result<response>> get_result(
            std::string_view url_str, coro::cancel_token token = {}) {
        return request_result(method::GET, url_str, "", "", std::move(token));
    }

    /// Owns request, target, and handler before lazy execution. Keep this client
    /// alive and unmoved through awaited return. The handler receives final
    /// headers (response.body() is empty), a scoped body reader, and the token.
    /// The token argument is read-only; by-value handlers receive a copy.
    /// It must await all reads before returning. Early return closes rather
    /// than drains; exceptions close the exchange and propagate unchanged.
    /// read_timeout retains its original per-hop I/O deadline across body
    /// pulls, but does not preempt handler code or include DNS/connect time.
    template<typename Handler>
        requires std::invocable<Handler&, const response&, response_body_reader&,
                                const coro::cancel_token&> &&
                 std::same_as<std::invoke_result_t<Handler&, const response&,
                     response_body_reader&, const coro::cancel_token&>, coro::task<void>>
    coro::task<client_result<std::monostate>> with_response(
            request req, url target, coro::cancel_token token, Handler handler,
            streaming_response_options options = {}) {
        return with_response_impl(std::move(req), std::move(target), std::move(token),
                                  std::move(handler), options);
    }

    /// Inspect sealed transport TLS diagnostics without exposing OpenSSL state.
    transport_tls_diagnostics tls_diagnostics() const noexcept {
        return transport_->tls_diagnostics();
    }

    /// Get configuration
    client_config& config() noexcept { return config_; }
    const client_config& config() const noexcept { return config_; }

    /// Method-general value API. Borrowed string inputs and this client must
    /// remain valid through awaited return; successful HTTP statuses are values.
    coro::task<client_result<response>> request_result(method m,
                                                          std::string_view url_str,
                                                          std::string_view body = {},
                                                          std::string_view content_type = {},
                                                          coro::cancel_token token = {}) {
        return request_url_result(m, url_str, body, content_type, std::move(token), true);
    }

private:
    coro::task<client_result<response>> request_url_result(
            method m, std::string_view url_str, std::string_view body,
            std::string_view content_type, coro::cancel_token token,
            bool honor_empty_representation) {
        // Check if already cancelled
        if (token.is_cancelled()) {
            co_return detail::make_client_error(ECANCELED, client_stage::target);
        }

        auto parsed = url::parse(url_str);
        if (!parsed) {
            co_return detail::make_client_error(EINVAL, client_stage::target);
        }
        if (!detail::is_supported_http_url_scheme(parsed->scheme)) {
            co_return detail::make_client_error(EINVAL, client_stage::target);
        }

        if (!config_.user_agent.empty() &&
            !detail::is_valid_header_value(config_.user_agent)) {
            co_return detail::make_client_error(EINVAL, client_stage::request);
        }
        if (!content_type.empty() &&
            !detail::is_valid_header_value(content_type)) {
            co_return detail::make_client_error(EINVAL, client_stage::request);
        }

        request req(m, parsed->path_with_query());
        req.set_host(parsed->host_authority());

        if (!body.empty() || (honor_empty_representation && !content_type.empty())) {
            req.set_body(body);
            if (!content_type.empty()) {
                req.set_content_type(content_type);
            }
        }

        if (!config_.user_agent.empty()) {
            req.set_header("User-Agent", config_.user_agent);
        }

        co_return co_await send_request(req, *parsed, 0, std::move(token));
    }

    static std::optional<response> optional_response(client_result<response> result) {
        if (const auto* error = std::get_if<client_error>(&result)) {
            errno = error->code.value();
            return std::nullopt;
        }
        return std::move(std::get<response>(result));
    }

    coro::task<std::optional<response>> request_url(method m, std::string_view url_str,
            std::string_view body, std::string_view content_type, coro::cancel_token token) {
        // Preserve legacy empty-body serialization; the value API honors an
        // explicit content type even for a zero-length representation.
        co_return optional_response(co_await request_url_result(
            m, url_str, body, content_type, std::move(token), false));
    }

    static bool is_informational_status(uint16_t code) noexcept {
        return code >= 100 && code < 200;
    }

    static client_stage response_read_stage(const response_decoder& decoder) noexcept {
        return decoder.headers_complete() && !is_informational_status(decoder.status_code())
            ? client_stage::body : client_stage::headers;
    }

    /// Write the whole buffer to the connection, optionally bounded by
    /// `io_deadline`. read_timeout doubles as the send deadline: a stalled
    /// write to a malicious server is the same liveness problem as a
    /// stalled read, so the same bound applies. The watchdog shutdown(2)s
    /// the fd to abort an in-flight write on timeout. Returns an owned error
    /// on failure and an empty optional on success.
    static coro::task<std::optional<client_error>>
    write_request_data(connection& conn, std::string_view data,
                       const url& target,
                       std::chrono::nanoseconds io_deadline,
                       bool deadline_enforced,
                       runtime::scheduler* sched,
                       const coro::cancel_token& token) {
        io::io_result write_result{};
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = detail::request_write_result_for_test.load(std::memory_order_acquire)) {
            write_result = hook(data);
        } else
#endif
        if (deadline_enforced) {
            auto timed_out = std::make_shared<std::atomic<bool>>(false);
            write_result = co_await detail::await_fd_operation_with_watchdog(
                [&] { return conn.write_all(data, token); },
                sched, conn.fd(), io_deadline, timed_out);
            if (timed_out->load(std::memory_order_acquire)) {
                conn.mark_externally_shut_down();
                ELIO_LOG_ERROR("Write to {}:{} timed out after {}s",
                               target.host, target.effective_port(),
                               std::chrono::duration_cast<std::chrono::seconds>(io_deadline).count());
                co_return detail::make_client_error(ETIMEDOUT, client_stage::request);
            }
        } else {
            write_result = co_await conn.write_all(data, token);
        }
        if (write_result.result <= 0) {
            if (write_result.result == -ECANCELED && token.is_cancelled()) {
                detail::abort_stream_io(conn);
                co_return detail::make_client_error(ECANCELED, client_stage::request);
            }
            ELIO_LOG_ERROR("Failed to send request: {}",
                           write_result.result == 0 ? "connection closed"
                                                    : strerror(-write_result.result));
            co_return detail::make_client_error(
                write_result.result == 0 ? ECONNRESET : -write_result.result,
                client_stage::request);
        }
        co_return std::nullopt;
    }

    static bool is_auto_follow_redirect(status s) noexcept {
        switch (s) {
        case status::moved_permanently:
        case status::found:
        case status::see_other:
        case status::temporary_redirect:
        case status::permanent_redirect:
            return true;
        default:
            return false;
        }
    }

    class exchange_state final {
    public:
        exchange_state(transport::connection_lease lease,
                       const client_config& config, const url& target,
                       method request_method, std::string_view request_body, bool defer_body,
                       size_t informational_limit)
            : lease_(std::move(lease)), conn_(lease_.stream()),
              config_(config), target_(target),
              reader_(config.read_buffer_size), request_method_(request_method),
              request_body_(request_body), informational_limit_(informational_limit),
              body_pending_(defer_body),
              scheduler_(runtime::scheduler::current()),
              deadline_enforced_(scheduler_ && config.read_timeout.count() > 0),
              io_deadline_(config.read_timeout),
              response_deadline_(std::chrono::steady_clock::now() + io_deadline_),
              expect_bounded_(scheduler_ && config.expect_continue_timeout.count() > 0) {
            reader_.set_max_headers(config.max_headers);
            reader_.set_max_header_size(config.max_header_size);
            reader_.set_request_method(request_method_);
        }

        exchange_state(const exchange_state&) = delete;
        exchange_state& operator=(const exchange_state&) = delete;
        exchange_state(exchange_state&&) = delete;
        exchange_state& operator=(exchange_state&&) = delete;

        coro::task<std::optional<client_error>> send_initial(
                std::string_view data, coro::cancel_token token) {
            if (auto error = co_await write_request_data(conn_, data, target_,
                    io_deadline_, deadline_enforced_, scheduler_, token)) {
                co_return error;
            }
            expect_deadline_ = std::chrono::steady_clock::now() +
                config_.expect_continue_timeout;
            if (body_pending_ && !expect_bounded_) {
                co_return co_await send_pending_body(token);
            }
            co_return std::nullopt;
        }

        coro::task<client_result<response_read_result>> next(coro::cancel_token token) {
            auto receive = [this, token](void* data, size_t size) {
                return receive_data(data, size, token);
            };
            while (true) {
                if (token.is_cancelled()) {
                    co_return detail::make_client_error(ECANCELED,
                        response_read_stage(reader_.decoder()));
                }
                auto part = co_await reader_.read_with(receive, token);
                if (fallback_error_) co_return *fallback_error_;
                if (!part.success()) {
                    if (expect_expired_ && body_pending_ && !token.is_cancelled()) {
                        if (auto error = co_await send_pending_body(token)) co_return *error;
                        continue;
                    }
                    const auto stage = reader_.decoder().has_error() ? client_stage::framing
                        : response_read_stage(reader_.decoder());
                    co_return detail::make_client_error(part.error, stage);
                }
                const auto code = reader_.decoder().status_code();
                if (part.event == response_event::headers_complete) {
#ifdef ELIO_RUNTIME_TEST_HOOKS
                    if (auto hook = detail::response_headers_for_test.load(std::memory_order_acquire)) {
                        hook(code);
                    }
                    if (!is_informational_status(code) &&
                        detail::expire_expect_after_headers_for_test.load(std::memory_order_acquire)) {
                        expect_deadline_ = std::chrono::steady_clock::now();
                        detail::client_response_read_staged_for_test.store(false, std::memory_order_release);
                        detail::final_headers_seen_for_test.store(true, std::memory_order_release);
                    }
#endif
                    if (code == static_cast<uint16_t>(status::switching_protocols) ||
                        (request_method_ == method::CONNECT && code >= 200 && code < 300)) {
                        co_return detail::make_client_error(EBADMSG, client_stage::framing);
                    }
                    if (!is_informational_status(code)) {
                        body_pending_ = false;
                        co_return part;
                    }
                    if (code == static_cast<uint16_t>(status::continue_)) {
                        if (auto error = co_await send_pending_body(token)) co_return *error;
                    }
                } else if (part.event == response_event::protocol_handoff) {
                    co_return detail::make_client_error(EBADMSG, client_stage::framing);
                } else if (part.event == response_event::message_complete) {
                    if (!is_informational_status(code)) co_return part;
                    if (reader_.message_bytes() > informational_limit_ -
                        skipped_informational_bytes_) {
                        co_return detail::make_client_error(EMSGSIZE, client_stage::framing);
                    }
                    skipped_informational_bytes_ += reader_.message_bytes();
                    if (!reader_.next_response()) {
                        co_return detail::make_client_error(EBADMSG, client_stage::framing);
                    }
                    reader_.set_request_method(request_method_);
                } else if (part.event == response_event::body) {
                    co_return part;
                }
            }
        }

        const response_reader& reader() const noexcept { return reader_; }

        void finish(const coro::cancel_token& token) {
            if (!token.is_cancelled() && reusable()) lease_.return_reusable();
            else lease_.retire();
        }
        void abort() noexcept { lease_.retire(); }

    private:
        bool reusable() const {
            return reader_.decoder().is_complete() &&
                !reader_.decoder().is_close_delimited() && !reader_.reached_eof() &&
                reader_.bytes_remaining() == 0 &&
                reader_.decoder().get_headers().keep_alive(reader_.decoder().version());
        }

        coro::task<std::optional<client_error>> send_pending_body(
                const coro::cancel_token& token) {
            if (!body_pending_) co_return std::nullopt;
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (auto hook = detail::deferred_upload_for_test.load()) hook(conn_.fd());
#endif
            body_pending_ = false;
            const auto remaining = response_deadline_ - std::chrono::steady_clock::now();
            if (deadline_enforced_ && remaining <= std::chrono::steady_clock::duration::zero()) {
                co_return detail::make_client_error(ETIMEDOUT, client_stage::request);
            }
            auto result = co_await write_request_data(conn_, request_body_, target_,
                deadline_enforced_ ? remaining : io_deadline_, deadline_enforced_, scheduler_, token);
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (auto hook = detail::deferred_upload_result_for_test.load()) co_await hook(result);
#endif
            co_return result;
        }

        coro::task<io::io_result> receive_data(void* data, size_t size,
                                             coro::cancel_token token) {
            expect_expired_ = false;
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (auto hook = detail::response_deadline_for_test.load(std::memory_order_acquire)) {
                hook(response_deadline_);
            }
#endif
            const auto now = std::chrono::steady_clock::now();
            if (deadline_enforced_ && now >= response_deadline_) {
                co_return io::io_result{-ETIMEDOUT, 0};
            }
            const bool waiting_expect = body_pending_ && expect_bounded_;
            if (waiting_expect && now >= expect_deadline_) {
                expect_expired_ = true;
                co_return io::io_result{-ETIMEDOUT, 0};
            }
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (detail::observe_client_response_read_entry_for_test.load(std::memory_order_acquire)) {
                detail::client_response_read_staged_for_test.store(false, std::memory_order_release);
                detail::response_read_stage_for_test.store(
                    response_read_stage(reader_.decoder()), std::memory_order_release);
            }
            detail::arm_client_response_read_observer_for_test();
#endif
            if (!deadline_enforced_ && !waiting_expect) {
                co_return co_await conn_.read(data, size, token);
            }
            const bool expect_first = waiting_expect &&
                (!deadline_enforced_ || expect_deadline_ < response_deadline_);
            const auto deadline = expect_first ? expect_deadline_ : response_deadline_;
            auto read_cancel = std::make_shared<coro::cancel_source>();
            auto forward = token.on_cancel([read_cancel] { read_cancel->cancel(); });
            auto expired = std::make_shared<std::atomic<bool>>(false);
            auto read_completed = std::make_shared<std::atomic<bool>>(false);
            auto read_failed = std::make_shared<std::atomic<bool>>(false);
            coro::cancel_source watchdog_cancel;
            auto watchdog = scheduler_->go_joinable(
                [this, deadline, expect_first, expired, read_cancel, read_completed, read_failed,
                 stage = response_read_stage(reader_.decoder()),
                 stop = watchdog_cancel.get_token()]() -> coro::task<void> {
                    try {
                        coro::cancel_result result;
                        const auto remaining = deadline - std::chrono::steady_clock::now();
#ifdef ELIO_RUNTIME_TEST_HOOKS
                        if (auto hook = detail::response_watchdog_wait_for_test.load(
                                std::memory_order_acquire)) {
                            result = co_await hook(remaining, stop, stage);
                        } else
#endif
                        {
                            (void)stage;
                            result = co_await elio::time::sleep_for(remaining, stop);
                        }
                        if (result != coro::cancel_result::completed ||
                            read_completed->load(std::memory_order_acquire)) co_return;
                        if (expect_first) {
                            // Expect expiry starts the deferred upload alongside
                            // the healthy read; cancelling TLS here is terminal.
                            if (auto error = co_await send_pending_body(read_cancel->get_token())) {
                                // The upload deadline can terminate the read first;
                                // retain that timeout, not the sibling's abort error.
                                if (error->code.value() == ETIMEDOUT ||
                                    !read_failed->load(std::memory_order_acquire))
                                    fallback_error_ = std::move(*error);
                                read_cancel->cancel();
                                co_return;
                            }
                            if (!deadline_enforced_ ||
                                read_completed->load(std::memory_order_acquire)) co_return;
                            const auto response_remaining = response_deadline_ -
                                std::chrono::steady_clock::now();
                            if (response_remaining > std::chrono::steady_clock::duration::zero()) {
#ifdef ELIO_RUNTIME_TEST_HOOKS
                                if (auto hook = detail::response_watchdog_wait_for_test.load(
                                        std::memory_order_acquire)) {
                                    result = co_await hook(response_remaining, stop, stage);
                                } else
#endif
                                {
                                    result = co_await elio::time::sleep_for(response_remaining, stop);
                                }
                                if (result != coro::cancel_result::completed ||
                                    read_completed->load(std::memory_order_acquire)) co_return;
                            }
                        }
                        expired->store(true, std::memory_order_release);
                        read_cancel->cancel();
                    } catch (...) {
                        // Timer and fallback-write exceptions both release the
                        // sibling read without replacing the first exception.
                        auto failure = std::current_exception();
                        try { read_cancel->cancel(); } catch (...) {}
                        std::rethrow_exception(failure);
                    }
                });
            io::io_result result{};
            std::exception_ptr failure;
            try {
                result = co_await conn_.read(data, size, read_cancel->get_token());
            } catch (...) {
                failure = std::current_exception();
            }
            if (failure || result.result <= 0) {
                // A failed read stops a pending upload, but its cancellation
                // result must not replace the original read failure.
                read_failed->store(true, std::memory_order_release);
                try { read_cancel->cancel(); }
                catch (...) { if (!failure) failure = std::current_exception(); }
            }
            read_completed->store(true, std::memory_order_release);
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (auto hook = detail::response_transport_result_for_test.load()) hook(result);
#endif
            try {
                watchdog_cancel.cancel();
            } catch (...) {
                if (!failure) failure = std::current_exception();
            }
            try {
                co_await watchdog;
            } catch (...) {
                if (!failure) failure = std::current_exception();
            }
            try {
                co_await watchdog.wait_destroyed_async();
            } catch (...) {
                if (!failure) failure = std::current_exception();
            }
            if (failure) std::rethrow_exception(failure);
            if (expired->load(std::memory_order_acquire)) {
                co_return io::io_result{-ETIMEDOUT, 0};
            }
            co_return result;
        }

        transport::connection_lease lease_;
        connection& conn_;
        const client_config& config_;
        const url& target_;
        response_reader reader_;
        method request_method_;
        std::string_view request_body_;
        size_t informational_limit_;
        size_t skipped_informational_bytes_ = 0;
        bool body_pending_;
        runtime::scheduler* scheduler_;
        bool deadline_enforced_;
        std::chrono::nanoseconds io_deadline_;
        std::chrono::steady_clock::time_point response_deadline_;
        bool expect_bounded_;
        std::chrono::steady_clock::time_point expect_deadline_{};
        bool expect_expired_ = false;
        std::optional<client_error> fallback_error_;
    };

    coro::task<client_result<std::unique_ptr<exchange_state>>> prepare_exchange(
            request& req, const url& target, coro::cancel_token token,
            size_t informational_limit) {
        if (token.is_cancelled()) {
            co_return detail::make_client_error(ECANCELED, client_stage::acquire);
        }
        if (!detail::is_valid_url_input(target.host_authority()) ||
            !detail::is_valid_request_target(req.path_with_query())) {
            ELIO_LOG_ERROR("Invalid outbound HTTP request target");
            co_return detail::make_client_error(EINVAL, client_stage::target);
        }
        auto acquired = co_await transport_->acquire_leased_result(
            target, config_.connect_timeout, token);
        if (const auto* error = std::get_if<client_error>(&acquired)) co_return *error;
        auto lease = std::move(std::get<transport::connection_lease>(acquired));
        if (req.header("Host").empty()) req.set_host(target.host_authority());
        if (!req.get_headers().contains("Connection")) {
            req.set_header("Connection", "keep-alive");
        }
        const bool defer_body = req.expect_continue() && !req.body().empty();
        std::string request_data;
        try {
            request_data = defer_body ? req.serialize_headers() : req.serialize();
        } catch (const std::invalid_argument& ex) {
            ELIO_LOG_ERROR("Invalid outbound HTTP request: {}", ex.what());
            co_return detail::make_client_error(EINVAL, client_stage::request);
        }
        ELIO_LOG_DEBUG("Sending HTTP request to {}:{}", target.host, target.effective_port());
        if (token.is_cancelled()) {
            co_return detail::make_client_error(ECANCELED, client_stage::request);
        }
        auto exchange = std::make_unique<exchange_state>(std::move(lease),
            config_, target, req.get_method(),
            req.body(), defer_body, informational_limit);
        if (auto error = co_await exchange->send_initial(request_data, token)) co_return *error;
        co_return std::move(exchange);
    }

    struct redirect_request {
        request req;
        url target;
    };

    std::optional<redirect_request> make_redirect(
            const request& req, const url& target, const response& resp,
            size_t redirect_count) const {
        if (!config_.follow_redirects || !is_auto_follow_redirect(resp.get_status()) ||
            redirect_count >= config_.max_redirects) return std::nullopt;
        auto location = resp.header("Location");
        if (location.empty()) return std::nullopt;
        if (!detail::is_valid_url_input(location)) {
            ELIO_LOG_WARNING("Rejecting invalid HTTP redirect Location");
            return std::nullopt;
        }
        auto redirect_url = url::resolve_reference(target, location);
        if (!redirect_url) return std::nullopt;
        if (!detail::is_supported_http_url_scheme(redirect_url->scheme)) {
            ELIO_LOG_WARNING("Rejecting unsupported HTTP redirect scheme");
            return std::nullopt;
        }
        if (target.is_secure() && !redirect_url->is_secure()) {
            ELIO_LOG_WARNING("Rejecting insecure redirect from HTTPS to HTTP");
            return std::nullopt;
        }
        ELIO_LOG_DEBUG("Following HTTP redirect to {}:{}", redirect_url->host,
                       redirect_url->effective_port());
        method redirect_method = req.get_method();
        if ((resp.get_status() == status::see_other && req.get_method() != method::HEAD) ||
            ((resp.get_status() == status::moved_permanently || resp.get_status() == status::found) &&
             req.get_method() == method::POST)) {
            redirect_method = method::GET;
        }
        request redirect_req(redirect_method, redirect_url->path_with_query());
        redirect_req.set_host(redirect_url->host_authority());
        if (!config_.user_agent.empty()) redirect_req.set_header("User-Agent", config_.user_agent);
        const bool method_preserved = redirect_method == req.get_method() &&
            resp.get_status() != status::see_other;
        if (resp.get_status() == status::temporary_redirect ||
            resp.get_status() == status::permanent_redirect || method_preserved) {
            if (!req.body().empty()) {
                redirect_req.set_body(req.body());
            } else if (req.get_headers().content_length() == size_t{0}) {
                redirect_req.set_header("Content-Length", req.get_headers().get("Content-Length"));
            }
            if (!req.content_type().empty()) redirect_req.set_content_type(req.content_type());
            if (!req.body().empty() && req.expect_continue()) redirect_req.set_expect_continue();
        }
        return redirect_request{std::move(redirect_req), std::move(*redirect_url)};
    }

    template<typename Handler>
    coro::task<client_result<std::monostate>> with_response_impl(
            request req, url target, coro::cancel_token token, Handler handler,
            streaming_response_options options) {
        if (!detail::is_supported_http_url_scheme(target.scheme)) {
            co_return detail::make_client_error(EINVAL, client_stage::target);
        }
        for (size_t redirects = 0;; ++redirects) {
            auto prepared = co_await prepare_exchange(req, target, token,
                                                      options.max_informational_bytes);
            if (const auto* error = std::get_if<client_error>(&prepared)) co_return *error;
            auto exchange = std::move(std::get<std::unique_ptr<exchange_state>>(prepared));
            while (true) {
                auto next = co_await exchange->next(token);
                if (const auto* error = std::get_if<client_error>(&next)) {
                    exchange->abort();
                    co_return *error;
                }
                if (std::get<response_read_result>(next).event == response_event::headers_complete) break;
            }
            auto head = response::from_decoder(exchange->reader().decoder(), {});
            if (auto redirect = make_redirect(req, target, head, redirects)) {
                // The state borrows the current request/target; retire it before
                // replacing either, and never pool an unconsumed redirect body.
                exchange->abort();
                exchange.reset();
                req = std::move(redirect->req);
                target = std::move(redirect->target);
                continue;
            }
            response_body_reader body(
                [state = exchange.get()](coro::cancel_token read_token) {
                    return state->next(std::move(read_token));
                }, token, options.max_body_size);
            std::exception_ptr failure;
            try {
                co_await std::invoke(handler, std::as_const(head), body, std::as_const(token));
            } catch (...) {
                failure = std::current_exception();
            }
            if (failure) {
                exchange->abort();
                std::rethrow_exception(failure);
            }
            if (body.error()) {
                exchange->abort();
                co_return *body.error();
            }
            if (token.is_cancelled()) {
                exchange->abort();
                co_return detail::make_client_error(ECANCELED, client_stage::body);
            }
            if (body.complete()) {
                exchange->finish(token);
            } else {
                exchange->abort();
            }
            co_return std::monostate{};
        }
    }

    /// Send request with redirect handling
    coro::task<client_result<response>> send_request(request& req, const url& target,
                                                           size_t redirect_count,
                                                           coro::cancel_token token) {
        auto prepared = co_await prepare_exchange(req, target, token, config_.max_response_size);
        if (const auto* error = std::get_if<client_error>(&prepared)) co_return *error;
        auto exchange = std::move(std::get<std::unique_ptr<exchange_state>>(prepared));
        std::string response_body;

        while (true) {
            auto next = co_await exchange->next(token);
            if (const auto* error = std::get_if<client_error>(&next)) co_return *error;
            auto part = std::get<response_read_result>(next);
            if (part.event == response_event::body) {
                if (part.body.size() > config_.max_response_size -
                    std::min(response_body.size(), config_.max_response_size)) {
                    co_return detail::make_client_error(EMSGSIZE, client_stage::body);
                }
                response_body.append(part.body);
            } else if (part.event == response_event::message_complete) {
                break;
            }
        }
        auto resp = response::from_decoder(exchange->reader().decoder(), std::move(response_body));

        // Return connection to pool only when (a) keep-alive is allowed by
        // the response, (b) the parser is in the complete state, and (c) no
        // bytes are left over in the parser's input buffer. A non-empty
        // remaining buffer means the server pipelined extra bytes after the
        // response — pooling the conn would let those bytes be misread as the
        // head of the next response (response-splitting). On any failure of
        // these conditions the lease retires the connection instead.
        exchange->finish(token);

        if (auto redirect = make_redirect(req, target, resp, redirect_count)) {
            co_return co_await send_request(redirect->req, redirect->target,
                                          redirect_count + 1, token);
        }

        co_return resp;
    }

    client_config config_;
    std::shared_ptr<transport> transport_;
};

/// Simple convenience functions for one-off requests

/// Perform HTTP GET request
/// @return Response on success, std::nullopt on error (check errno)
inline coro::task<std::optional<response>> get(std::string_view url) {
    client c;
    co_return co_await c.get(url);
}

/// Perform HTTP GET request with cancellation support
inline coro::task<std::optional<response>> get(std::string_view url, coro::cancel_token token) {
    client c;
    co_return co_await c.get(url, std::move(token));
}

/// Perform HTTP POST request
/// @return Response on success, std::nullopt on error (check errno)
inline coro::task<std::optional<response>> post(std::string_view url,
                                                std::string_view body,
                                                std::string_view content_type = mime::application_form_urlencoded) {
    client c;
    co_return co_await c.post(url, body, content_type);
}

/// Perform HTTP POST request with cancellation support
inline coro::task<std::optional<response>> post(std::string_view url,
                                                std::string_view body,
                                                coro::cancel_token token,
                                                std::string_view content_type = mime::application_form_urlencoded) {
    client c;
    co_return co_await c.post(url, body, std::move(token), content_type);
}

} // namespace elio::http
