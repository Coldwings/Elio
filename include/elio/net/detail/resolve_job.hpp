#pragma once

#include <elio/net/resolve.hpp>

#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace elio::net::detail {

class dns_admission_state final {
public:
    explicit dns_admission_state(size_t limit) noexcept : limit_(limit) {}

    [[nodiscard]] bool try_acquire() noexcept {
        auto count = outstanding_.load(std::memory_order_acquire);
        while (count < limit_) {
            if (outstanding_.compare_exchange_weak(count, count + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    void release() noexcept {
        outstanding_.fetch_sub(1, std::memory_order_acq_rel);
    }

    [[nodiscard]] size_t outstanding() const noexcept {
        return outstanding_.load(std::memory_order_acquire);
    }

private:
    const size_t limit_;
    std::atomic<size_t> outstanding_{0};
};

class dns_admission_lease final {
public:
    dns_admission_lease() = default;
    dns_admission_lease(dns_admission_lease&& other) noexcept
        : state_(std::move(other.state_)) {}
    dns_admission_lease(const dns_admission_lease&) = delete;
    dns_admission_lease& operator=(const dns_admission_lease&) = delete;
    dns_admission_lease& operator=(dns_admission_lease&&) = delete;

    ~dns_admission_lease() {
        if (state_) state_->release();
    }

    [[nodiscard]] static dns_admission_lease try_acquire(
            std::shared_ptr<dns_admission_state> state) noexcept {
        if (!state || !state->try_acquire()) return {};
        return dns_admission_lease(std::move(state));
    }

    explicit operator bool() const noexcept { return state_ != nullptr; }

private:
    explicit dns_admission_lease(std::shared_ptr<dns_admission_state> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<dns_admission_state> state_;
};

class dns_job_state final {
public:
    enum class phase { queued, running, discarded, retired };

    dns_job_state()
        : result_(std::make_shared<coro::detail::join_state<dns_lookup_result>>()) {}

    [[nodiscard]] bool begin_lookup() noexcept {
        auto expected = phase::queued;
        return phase_.compare_exchange_strong(expected, phase::running,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    void depart() noexcept {
        auto expected = phase::queued;
        phase_.compare_exchange_strong(expected, phase::discarded,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    void retire() noexcept { phase_.store(phase::retired, std::memory_order_release); }

    [[nodiscard]] std::shared_ptr<coro::detail::join_state<dns_lookup_result>>
    result_state() const noexcept { return result_; }

#ifdef ELIO_RUNTIME_TEST_HOOKS
    [[nodiscard]] phase phase_for_test() const noexcept {
        return phase_.load(std::memory_order_acquire);
    }
#endif

private:
    std::shared_ptr<coro::detail::join_state<dns_lookup_result>> result_;
    std::atomic<phase> phase_{phase::queued};
};

#ifdef ELIO_RUNTIME_TEST_HOOKS
using owned_dns_lookup_hook = dns_lookup_result(*)(std::string_view, uint16_t);
inline std::atomic<owned_dns_lookup_hook> owned_dns_lookup_for_test{nullptr};
#endif

class owned_dns_job final {
public:
    owned_dns_job(dns_admission_lease lease,
                  std::unique_ptr<dns_lookup_request> request)
        : lease_(std::move(lease)), request_(std::move(request)),
          state_(std::make_shared<dns_job_state>()) {}

    [[nodiscard]] std::shared_ptr<dns_job_state> state() const noexcept { return state_; }

    void run() noexcept {
        if (!state_->begin_lookup()) {
            state_->retire();
            return;
        }

        std::optional<dns_lookup_result> result;
        std::exception_ptr failure;
        try {
            dns_lookup_operation operation(std::move(request_));
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (auto hook = owned_dns_lookup_for_test.load(std::memory_order_acquire)) {
                result.emplace(hook(operation.request->host, operation.request->port));
            } else
#endif
            {
                result.emplace(operation());
            }
        } catch (...) {
            failure = std::current_exception();
        }

        // Only lookup work is inside the exception boundary. Publish exactly
        // once; never overwrite a result after completion has become visible.
        auto completion = state_->result_state();
        if (failure) {
            completion->set_exception(std::move(failure));
        } else {
            completion->set_value(std::move(*result));
        }
        state_->retire();
    }

private:
    // Reverse member destruction frees the work-owned result/input before
    // releasing capacity. Observers retain state, never this work object.
    dns_admission_lease lease_;
    std::unique_ptr<dns_lookup_request> request_;
    std::shared_ptr<dns_job_state> state_;
};

inline std::shared_ptr<owned_dns_job> try_make_owned_dns_job(
        std::string_view host, uint16_t port,
        std::shared_ptr<dns_admission_state> admission) {
    auto lease = dns_admission_lease::try_acquire(std::move(admission));
    if (!lease) return {};
    auto request = std::make_unique<dns_lookup_request>(
        dns_lookup_request{std::string(host), port});
    return std::make_shared<owned_dns_job>(std::move(lease), std::move(request));
}

} // namespace elio::net::detail
