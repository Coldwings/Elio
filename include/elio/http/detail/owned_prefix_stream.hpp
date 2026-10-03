#pragma once

#include <elio/net/byte_stream.hpp>
#include <elio/net/tcp.hpp>
#include <elio/sync/detail/wake_state.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <exception>
#include <memory>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace elio::http::detail {

struct prefix_idle_access;

// CONNECT owns this prefix; lower EOF and readiness cannot skip it. The TCP
// root is adapted explicitly, not opted into the publishing concept globally.
template<typename Lower>
requires (std::same_as<Lower, net::tcp_stream> || net::publishing_byte_stream<Lower>)
class owned_prefix_stream {
    friend struct prefix_idle_access;
    struct state {
        state(Lower value, std::vector<char> bytes, std::shared_ptr<void> lifetime)
            : retirement(std::move(lifetime)), lower(std::move(value)), prefix(std::move(bytes)) {}

        int begin(bool write) {
            std::lock_guard lock(mutex);
            if (sealed) return ECANCELED;
            auto& side = write ? writing : reading;
            if (side) return EBUSY;
            side = true;
            ++active;
            return 0;
        }

        void finish(bool write) noexcept {
            sync::detail::wake_state_ptr wake;
            {
                std::lock_guard lock(mutex);
                (write ? writing : reading) = false;
                assert(active > 0);
                if (--active == 0) wake = std::move(abort_waiter);
            }
            // A selected wake owns no frame storage. Dispatch outside the lock
            // and without allocation, including exceptional operation cleanup.
            sync::detail::schedule_wake_state(wake);
        }

        void request_abort() noexcept {
            {
                std::lock_guard lock(mutex);
                sealed = true;
            }
            try { stop.cancel(); } catch (...) {}
            if constexpr (requires(Lower& stream) { { stream.shutdown_socket() } noexcept; })
                lower.shutdown_socket();
        }

        // Physical lower closure precedes permit/operation-owner release,
        // including when a TLS output pump retains this state after retirement.
        std::shared_ptr<void> retirement;
        Lower lower;
        std::vector<char> prefix;
        size_t offset = 0;
        coro::cancel_source stop;
        std::mutex mutex;
        sync::detail::wake_state_ptr abort_waiter;
        size_t active = 0;
        bool reading = false;
        bool writing = false;
        bool sealed = false;
    };

    struct operation {
        std::shared_ptr<state> owner;
        bool write;
        ~operation() { owner->finish(write); }
    };

    class abort_wait {
    public:
        abort_wait(std::shared_ptr<state> owner, sync::detail::wake_state_ptr wake) noexcept
            : owner_(std::move(owner)), wake_(std::move(wake)) {}
        ~abort_wait() {
            std::lock_guard lock(owner_->mutex);
            if (owner_->abort_waiter == wake_) owner_->abort_waiter.reset();
            wake_->abandon();
        }
        bool await_ready() const noexcept {
            std::lock_guard lock(owner_->mutex);
            return owner_->active == 0;
        }
        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            auto owner = owner_;
            auto wake = wake_;
            std::lock_guard lock(owner->mutex);
            if (owner->active == 0) return false;
            assert(!owner->abort_waiter && "serialize multiple CONNECT channel aborts");
            if (!wake->set_handle_blocked(handle)) return false;
            owner->abort_waiter = wake;
            return wake->unblock_after_publish();
        }
        void await_resume() const noexcept {
            std::lock_guard lock(owner_->mutex);
            assert(owner_->active == 0);
        }

    private:
        std::shared_ptr<state> owner_;
        sync::detail::wake_state_ptr wake_;
    };

public:
    using byte_stream_contract = net::publishing_byte_stream_contract;
    static constexpr bool tls_progress_interrupts_read = [] {
        if constexpr (std::same_as<Lower, net::tcp_stream>) return true;
        else if constexpr (requires { Lower::tls_progress_interrupts_read; })
            return static_cast<bool>(Lower::tls_progress_interrupts_read);
        else return false;
    }();

    owned_prefix_stream(Lower lower, std::vector<char> prefix, size_t prefix_limit,
                        std::shared_ptr<void> retirement = {}) {
        if constexpr (std::same_as<Lower, net::tcp_stream>) {
            if (retirement) net::detail::tcp_retirement_access::mark_settled_root(lower);
        }
        if (prefix.size() > prefix_limit)
            throw std::invalid_argument("HTTP CONNECT read-ahead exceeds its bound");
        owner_ = std::make_shared<state>(std::move(lower), std::move(prefix), std::move(retirement));
    }
    owned_prefix_stream(owned_prefix_stream&&) noexcept = default;
    owned_prefix_stream& operator=(owned_prefix_stream&& other) noexcept {
        if (this != &other) {
            shutdown_socket();
            owner_ = std::move(other.owner_);
        }
        return *this;
    }
    owned_prefix_stream(const owned_prefix_stream&) = delete;
    owned_prefix_stream& operator=(const owned_prefix_stream&) = delete;
    ~owned_prefix_stream() { shutdown_socket(); }

    coro::task<io::io_result> read(void* data, size_t size, coro::cancel_token token) {
        return read_owned(owner_, data, size, std::move(token));
    }
    coro::task<io::io_result> write(const void* data, size_t size, coro::cancel_token token) {
        return write_owned(owner_, data, size, std::move(token));
    }
    coro::task<net::write_finish_result> finish_write(coro::cancel_token token,
            std::chrono::milliseconds timeout) {
        return finish_owned(owner_, std::move(token), timeout);
    }
    net::close_scope read_end_scope() const noexcept {
        return owner_ ? owner_->lower.read_end_scope() : net::close_scope::write_direction;
    }
    void shutdown_socket() noexcept {
        if (owner_) owner_->request_abort();
    }
    coro::task<void> abort_and_settle() {
        std::optional<coro::task<void>> lower_abort;
        if constexpr (net::publishing_byte_stream<Lower>) {
            if (owner_) lower_abort.emplace(owner_->lower.abort_and_settle());
        }
        // Allocate all wake/task state before starting abort. Once sealed, the
        // exceptional cleanup path can wait for owned sides without allocating.
        auto wake = sync::detail::make_wake_state();
        return abort_owned(owner_, std::move(wake), std::move(lower_abort));
    }

private:
    static coro::task<io::io_result> read_owned(std::shared_ptr<state> owner,
            void* data, size_t size, coro::cancel_token token) {
        if (!owner) co_return io::io_result{-ENOTCONN, 0};
        if (token.is_cancelled()) co_return io::io_result{-ECANCELED, 0};
        if (auto error = owner->begin(false)) co_return io::io_result{-error, 0};
        operation current{owner, false};
        if (size == 0) co_return io::io_result{0, 0};
        if (owner->offset < owner->prefix.size()) {
            const auto count = std::min({size, owner->prefix.size() - owner->offset,
                static_cast<size_t>(std::numeric_limits<int32_t>::max())});
            std::memcpy(data, owner->prefix.data() + owner->offset, count);
            owner->offset += count;
            co_return io::io_result{static_cast<int32_t>(count), 0};
        }
        coro::cancel_source stopped;
        auto abort = owner->stop.get_token().on_cancel([stopped]() mutable { stopped.cancel(); });
        auto user = token.on_cancel([stopped]() mutable { stopped.cancel(); });
        co_return co_await owner->lower.read(data, size, stopped.get_token());
    }

    static coro::task<io::io_result> write_owned(std::shared_ptr<state> owner,
            const void* data, size_t size, coro::cancel_token token) {
        if (!owner) co_return io::io_result{-ENOTCONN, 0};
        if (token.is_cancelled()) co_return io::io_result{-ECANCELED, 0};
        if (auto error = owner->begin(true)) co_return io::io_result{-error, 0};
        operation current{owner, true};
        coro::cancel_source stopped;
        auto abort = owner->stop.get_token().on_cancel([stopped]() mutable { stopped.cancel(); });
        auto user = token.on_cancel([stopped]() mutable { stopped.cancel(); });
        auto result = co_await owner->lower.write(data, size, stopped.get_token());
        if (size != 0 && result.result == 0) result.result = -EIO;
        co_return result;
    }

    static coro::task<net::write_finish_result> finish_owned(std::shared_ptr<state> owner,
            coro::cancel_token token, std::chrono::milliseconds timeout) {
        if (!owner) co_return net::write_finish_result{net::close_scope::write_direction, ENOTCONN};
        if (token.is_cancelled())
            co_return net::write_finish_result{owner->lower.read_end_scope(), ECANCELED};
        if (auto error = owner->begin(true))
            co_return net::write_finish_result{owner->lower.read_end_scope(), error};
        operation current{owner, true};
        coro::cancel_source stopped;
        auto abort = owner->stop.get_token().on_cancel([stopped]() mutable { stopped.cancel(); });
        auto user = token.on_cancel([stopped]() mutable { stopped.cancel(); });
        co_return co_await owner->lower.finish_write(stopped.get_token(), timeout);
    }

    static coro::task<void> abort_owned(std::shared_ptr<state> owner,
            sync::detail::wake_state_ptr wake, std::optional<coro::task<void>> lower_abort) {
        if (!owner) co_return;
        owner->request_abort();
        std::exception_ptr failure;
        if (lower_abort) {
            try { co_await std::move(*lower_abort); }
            catch (...) { failure = std::current_exception(); }
        }
        co_await abort_wait(owner, std::move(wake));
        if (failure) std::rethrow_exception(failure);
    }

    std::shared_ptr<state> owner_;
};

// Internal recursive pooling check. Public/lower frames must be gone before
// inspecting unread prefix and nested TLS state; inactivity alone is not enough.
struct prefix_idle_access {
#ifdef ELIO_RUNTIME_TEST_HOOKS
    template<typename Lower>
    static Lower& lower_for_test(owned_prefix_stream<Lower>& stream) noexcept {
        return stream.owner_->lower;
    }
#endif
    template<typename Lower, typename Check>
    static bool is_quiescent(const owned_prefix_stream<Lower>& stream, Check check) noexcept {
        if (!stream.owner_) return false;
        const auto& owner = stream.owner_;
        std::lock_guard lock(owner->mutex);
        return owner.use_count() == 1 && owner->active == 0 && !owner->sealed &&
            owner->offset == owner->prefix.size() && check(std::as_const(owner->lower));
    }
};

} // namespace elio::http::detail
