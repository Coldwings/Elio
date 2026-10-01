#pragma once

#include <elio/net/detail/resolve_job.hpp>
#include <elio/coro/join_wait.hpp>

#include <cerrno>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace elio::net {

namespace detail { struct resolve_domain_access; }

/// Shared admission for queued, running, and observer-departed DNS work.
/// A zero capacity admits no libc lookups; literals and cache hits are exempt.
class resolve_domain final {
    friend struct detail::resolve_domain_access;
public:
    explicit resolve_domain(size_t capacity = 64)
        : state_(std::make_shared<detail::dns_admission_state>(capacity)) {}

    [[nodiscard]] size_t capacity() const noexcept {
        return state_ ? state_->limit() : 0;
    }
    [[nodiscard]] size_t outstanding() const noexcept {
        return state_ ? state_->outstanding() : 0;
    }

private:
    std::shared_ptr<detail::dns_admission_state> state_;
};

inline std::shared_ptr<resolve_domain> default_resolve_domain() {
    static auto domain = std::make_shared<resolve_domain>();
    return domain;
}

enum class resolve_status { resolved, failed, cancelled, timed_out };

/// Owned operational outcome. error is positive errno, not ambient errno.
struct resolve_result {
    std::vector<socket_address> addresses;
    resolve_status status = resolve_status::failed;
    int error = EHOSTUNREACH;

    explicit operator bool() const noexcept {
        return status == resolve_status::resolved && !addresses.empty();
    }
};

struct resolve_wait_options {
    resolve_options lookup;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::shared_ptr<resolve_domain> domain;  ///< Null selects the shared default.
};

namespace detail {

struct resolve_domain_access final {
    static std::shared_ptr<dns_admission_state> state(const resolve_domain& domain) noexcept {
        return domain.state_;
    }
};

struct resolve_departure_guard final {
    std::shared_ptr<dns_job_state> state;
    ~resolve_departure_guard() { state->depart(); }
};

inline resolve_result resolve_failure(int error,
        resolve_status status = resolve_status::failed) {
    return {{}, status, error > 0 ? error : EIO};
}

inline coro::task<resolve_result> resolve_all_wait_owned(std::string host, uint16_t port,
        resolve_wait_options options, coro::cancel_token token) {
    // Entry cancellation/deadline takes precedence over unstarted fast paths.
    if (token.is_cancelled()) {
        co_return resolve_failure(ECANCELED, resolve_status::cancelled);
    }
    if (options.deadline && *options.deadline <= std::chrono::steady_clock::now()) {
        co_return resolve_failure(ETIMEDOUT, resolve_status::timed_out);
    }

    std::vector<socket_address> addresses;
    if (host.empty() || host == "::" || host == "0.0.0.0") {
        addresses.emplace_back(host, port);
        co_return resolve_result{std::move(addresses), resolve_status::resolved, 0};
    }
    if ((host.find(':') != std::string::npos && try_parse_ipv6_literal(host, port, addresses)) ||
            try_parse_ipv4_literal(host, port, addresses)) {
        co_return resolve_result{std::move(addresses), resolve_status::resolved, 0};
    }

    resolve_cache_key key{host, port};
    resolve_cache* cache = options.lookup.use_cache
        ? (options.lookup.cache ? options.lookup.cache : &default_resolve_cache()) : nullptr;
    if (cache) {
        std::optional<int> cached_error;
        if (cache->try_get(key, addresses, &cached_error)) {
            if (addresses.empty()) co_return resolve_failure(cached_error.value_or(EHOSTUNREACH));
            co_return resolve_result{std::move(addresses), resolve_status::resolved, 0};
        }
        cache->record_miss();
    }

    auto* scheduler = runtime::scheduler::current();
    if (!runtime::worker_thread::current() || !scheduler || !scheduler->is_running()) {
        co_return resolve_failure(ENOTSUP);
    }
    auto domain = options.domain ? options.domain : default_resolve_domain();
    auto job = try_make_owned_dns_job(host, port, resolve_domain_access::state(*domain));
    if (!job) co_return resolve_failure(EAGAIN);
    auto state = job->state();
    resolve_departure_guard departure{state};
    auto completion = state->result_state();
    std::function<void()> work = [job]() noexcept {
        // Generic pool shutdown may drain callbacks on its caller. Never enter
        // libc on a scheduler worker, including that teardown path.
        if (runtime::worker_thread::current()) job->reject(EAGAIN);
        else job->run();
    };
    auto* pool = scheduler->get_blocking_pool();
    if (!pool || !pool->submit_bounded(std::move(work), domain->capacity())) {
        co_return resolve_failure(EAGAIN);
    }
    work = nullptr;
    job.reset();

    auto outcome = co_await coro::detail::observe_join_result(
        completion, options.deadline, std::move(token));
    if (outcome == coro::join_wait_outcome::cancelled) {
        co_return resolve_failure(ECANCELED, resolve_status::cancelled);
    }
    if (outcome == coro::join_wait_outcome::timed_out) {
        co_return resolve_failure(ETIMEDOUT, resolve_status::timed_out);
    }
    auto result = completion->get_value();
    if (result.addresses.empty() && result.error <= 0) result.error = EHOSTUNREACH;
    // Only this live observer owns the borrowed cache. Producers never capture
    // it; departure prevents late publication even when libc later succeeds.
    if (cache && result.cacheable) {
        if (result.addresses.empty()) {
            cache->store(key, {}, options.lookup.negative_ttl, result.error);
        } else {
            cache->store(key, result.addresses, options.lookup.positive_ttl);
        }
    }
    if (result.addresses.empty()) co_return resolve_failure(result.error);
    co_return resolve_result{std::move(result.addresses), resolve_status::resolved, 0};
}

} // namespace detail

/// Copy the hostname before returning a lazy task. A borrowed custom cache must
/// outlive normal awaited return, not the background lookup after departure.
/// Completion/cancellation/deadline has one observer winner; running libc work
/// is not interrupted and retains capacity until reclamation. Overload is
/// EAGAIN; pending lookup needs a running scheduler worker. Setup/lookup
/// exceptions may propagate. Normal scheduler shutdown still drains producers.
/// An explicit fourth token argument preserves legacy three-argument overloads.
inline coro::task<resolve_result> resolve_all(std::string_view host, uint16_t port,
        resolve_wait_options options, coro::cancel_token token) {
    return detail::resolve_all_wait_owned(std::string(host), port,
        std::move(options), std::move(token));
}

} // namespace elio::net
