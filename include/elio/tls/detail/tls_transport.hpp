#pragma once

#include "output_bio.hpp"
#include "../../net/tcp.hpp"
#include "../../sync/mutex.hpp"
#include "../../sync/event.hpp"
#include "../../sync/detail/wake_state.hpp"
#include "../../coro/detail/completion_waiter.hpp"
#include "../../runtime/scheduler.hpp"

#include <openssl/bio.h>

#include <cstddef>
#include <cstdint>
#include <array>
#include <cassert>
#include <concepts>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace elio::tls::detail {

// A failed connector has one serialized physical-root observer. Reserve its
// completion slot before driving TLS; notification bookkeeping does not allocate,
// and abandoning an unclaimed wake cannot retain a stale handle. Scheduling the
// selected coroutine still uses the runtime's ordinary dispatch storage.
class tls_root_release_state : public std::enable_shared_from_this<tls_root_release_state> {
    class awaitable {
    public:
        explicit awaitable(std::shared_ptr<tls_root_release_state> owner) noexcept
            : owner_(std::move(owner)), waiter_(owner_->slot_) {}
        bool await_ready() const noexcept { return owner_->released_.load(std::memory_order_acquire); }
        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            auto owner = owner_;
            return owner->slot_.register_waiter(waiter_, handle, [owner] {
                return owner->released_.load(std::memory_order_acquire);
            });
        }
        void await_resume() const noexcept {
            assert(owner_->released_.load(std::memory_order_acquire));
        }

    private:
        std::shared_ptr<tls_root_release_state> owner_;
        coro::detail::completion_waiter waiter_;
    };

public:
    auto wait() noexcept { return awaitable(shared_from_this()); }
    void set() noexcept {
        released_.store(true, std::memory_order_release);
        auto selected = slot_.take();
        if (auto handle = selected.claim()) runtime::schedule_handle(handle);
    }

private:
    std::atomic<bool> released_{false};
    coro::detail::completion_waiter_slot slot_;
};

struct tls_root_retirement {
    std::shared_ptr<void> owner;
    std::shared_ptr<tls_root_release_state> released = std::make_shared<tls_root_release_state>();
    ~tls_root_retirement() {
        owner.reset();
        released->set();
    }
};

// Owns ciphertext and the lower stream, never SSL or caller plaintext. The TLS
// owner must call fail() on abandonment: an active pump deliberately retains
// this object until the lower layer has released its borrowed ciphertext lease.
template<typename Lower>
class basic_tls_transport : public std::enable_shared_from_this<basic_tls_transport<Lower>> {
    friend struct tls_idle_access;
    using self_type = basic_tls_transport<Lower>;
    using wake_ptr = sync::detail::wake_state_ptr;
    using wake_list = std::list<wake_ptr>;
    // The terminal contract permits one reader, one writer, and one abort.
    static constexpr size_t cleanup_capacity = 3;

    static consteval bool compute_progress_interrupts_read() {
        if constexpr (std::same_as<Lower, net::tcp_stream>) {
            return true;
        } else if constexpr (requires {
                                 { Lower::tls_progress_interrupts_read } -> std::convertible_to<bool>;
                             }) {
            return Lower::tls_progress_interrupts_read;
        } else {
            return false;
        }
    }

    static constexpr bool progress_interrupts_read = compute_progress_interrupts_read();

    struct launch_ticket {
        std::shared_ptr<self_type> owner;
        std::atomic<bool> entered{false};
        ~launch_ticket() {
            if (entered.load(std::memory_order_acquire)) return;
            // scheduler::go destroys rejected tasks without throwing.
            owner->fail(ECANCELED);
            {
                std::lock_guard lock(owner->mutex);
                owner->pump_active_ = false;
            }
            owner->notify_progress();
        }
    };

    class change_waiter {
    public:
        change_waiter(std::shared_ptr<self_type> owner, uint64_t observed,
                      coro::cancel_token token)
            : owner_(std::move(owner)), observed_(observed),
              wake_(sync::detail::make_wake_state()) {
            // Allocate both the queue node and cancellation registration before
            // publishing anything. Notifications only splice existing nodes.
            pending_.push_back(wake_);
            registration_ = token.on_cancel([wake = wake_] { wake->request_cancel(); });
        }
        ~change_waiter() {
            registration_.unregister();
            wake_->abandon();
            std::lock_guard lock(owner_->mutex);
            owner_->waiters_.remove(wake_);
        }
        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> handle) {
            auto wake = wake_;
            if (!wake->set_handle_blocked(handle)) return false;
            {
                std::lock_guard lock(owner_->mutex);
                if (owner_->generation != observed_) {
                    wake->claim_notification();
                } else {
                    owner_->waiters_.splice(owner_->waiters_.end(), pending_);
                }
            }
            return wake->unblock_after_publish();
        }
        io::io_result await_resume() const noexcept {
            return {wake_->was_cancelled() ? -ECANCELED : 0, 0};
        }
    private:
        std::shared_ptr<self_type> owner_;
        uint64_t observed_;
        wake_ptr wake_;
        wake_list pending_;
        coro::cancel_token::registration registration_;
    };

public:
    basic_tls_transport(Lower stream, size_t budget, int direct_output_fd = -1)
        : lower(std::move(stream)), output(direct_output_fd, budget) {}

    // Destroy the physical lower before releasing its caller's accounting.
    std::shared_ptr<tls_root_retirement> retirement;
    Lower lower;
    output_bio_state output;
    BIO* input = nullptr;
    std::mutex mutex;
    // Read or modify only under mutex. All methods below acquire mutex and
    // therefore must be called outside the SSL/state critical section.
    uint64_t generation = 0;

    // Only called under mutex, including the actual SSL dispatch section.
    int operation_error() const noexcept {
        return output.error() ? output.error() : output_retired_ ? ESHUTDOWN : 0;
    }

    void retire_output() noexcept {
        std::lock_guard lock(mutex);
        output_retired_ = true;
    }
#ifdef ELIO_RUNTIME_TEST_HOOKS
    struct output_test_state {
        size_t budget;
        size_t retained;
        size_t pending;
        uint64_t accepted;
        uint64_t drained;
        int error;
        bool pump_active;
    };

    void* read_publish_context = nullptr;
    void (*before_read_publish)(void*) = nullptr;
    void* output_progress_context = nullptr;
    coro::task<void> (*after_output_progress)(void*, uint64_t) = nullptr;
    coro::task<void> (*after_output_inactive)(void*, uint64_t) = nullptr;
    bool output_active_for_test() const noexcept { return pump_active_; }
    output_test_state output_state_for_test() noexcept {
        std::lock_guard lock(mutex);
        return {output.budget(), output.retained_bytes(), output.pending_bytes(),
                output.accepted_bytes(), output.drained_bytes(), output.error(),
                pump_active_};
    }
    void set_output_active_for_test(bool active) noexcept {
        std::lock_guard lock(mutex);
        pump_active_ = active;
    }
#endif

    void notify_progress() noexcept {
        wake_list ready;
        std::array<wake_ptr, cleanup_capacity> settled;
        std::shared_ptr<coro::cancel_source> reader;
        {
            std::lock_guard lock(mutex);
            ++generation;
            if constexpr (progress_interrupts_read) reader = read_poll_;
            ready.splice(ready.end(), waiters_);
            for (const auto& wake : ready) wake->claim_notification();
            if (!pump_active_) {
                for (size_t i = 0; i < cleanup_slots_.size(); ++i) {
                    auto& slot = cleanup_slots_[i];
                    if (!slot.registered) continue;
                    slot.registered = false;
                    settled[i] = wake_ptr(this->shared_from_this(), &slot.wake);
                    slot.wake.claim_notification();
                }
            }
        }
        cancel_noexcept(reader);
        for (const auto& wake : ready) wake->schedule_claimed();
        for (const auto& wake : settled) if (wake) wake->schedule_claimed();
    }

    coro::task<io::io_result> wait_change(uint64_t observed, coro::cancel_token token) {
        co_return co_await change_waiter(this->shared_from_this(), observed, std::move(token));
    }

    coro::task<io::io_result> wait_read(uint64_t observed, coro::cancel_token token) {
        auto keepalive = this->shared_from_this();
        (void)keepalive;
        if (co_await read_mutex_.lock(token) == coro::cancel_result::cancelled)
            co_return io::io_result{-ECANCELED, 0};
        sync::lock_guard guard(read_mutex_);
        auto source = std::make_shared<coro::cancel_source>();
        auto registration = token.on_cancel([source] { source->cancel(); });
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (before_read_publish) before_read_publish(read_publish_context);
#endif
        {
            std::lock_guard lock(mutex);
            if (output.error()) co_return io::io_result{-output.error(), 0};
            if (generation != observed) co_return io::io_result{0, 0};
            if (!input) { output.fail(EIO); co_return io::io_result{-EIO, 0}; }
            read_poll_ = source;
        }
        io::io_result result;
        std::array<std::byte, 16 * 1024> buffer{};
        try {
            result = co_await lower.read(buffer.data(), buffer.size(), source->get_token());
        } catch (...) {
            std::lock_guard lock(mutex);
            if (read_poll_ == source) read_poll_.reset();
            throw;
        }
        bool made_progress = false;
        {
            std::lock_guard lock(mutex);
            if (read_poll_ == source) read_poll_.reset();
            if (output.error()) {
                result = {-output.error(), 0};
            } else if (result.result > 0) {
                const int accepted = BIO_write(input, buffer.data(), result.result);
                if (accepted != result.result) {
                    output.fail(EIO);
                    result = {-output.error(), 0};
                } else {
                    result = {0, 0};
                    made_progress = true;
                }
            } else if (token.is_cancelled()) {
                result = {-ECANCELED, 0};
            } else if (source->is_cancelled() && result.result == -ECANCELED) {
                result = {0, 0};
            } else if (result.result == 0) {
                output.fail(EPIPE);
                result = {-EPIPE, 0};
            } else if (result.result < 0) {
                output.fail(-result.result);
            }
        }
        // The next logical reader must retry SSL before registering another
        // lower read: this ciphertext may already have been consumed by its sibling.
        if (made_progress || result.result >= 0) notify_progress();
        co_return result;
    }

    void start_output() noexcept {
        {
            std::lock_guard lock(mutex);
            if (pump_active_ || operation_error() || !output.pending_bytes()) return;
            pump_active_ = true;
        }
        int error = EIO;
        // Retain submission ownership across the catch. The scheduler may
        // destroy a rejected task inside its own catch before rethrowing.
        std::shared_ptr<launch_ticket> ticket;
        try {
            auto* scheduler = runtime::scheduler::current();
            if (!scheduler) throw std::runtime_error("TLS output requires a scheduler");
            auto self = this->shared_from_this();
            ticket = std::make_shared<launch_ticket>();
            ticket->owner = self;
            scheduler->go(pump(std::move(self), ticket));
            return;
        } catch (const std::bad_alloc&) {
            error = ENOMEM;
        } catch (...) {}
        fail(error);
        {
            std::lock_guard lock(mutex);
            pump_active_ = false;
        }
        notify_progress();
    }

    coro::task<io::io_result> wait_write(coro::cancel_token token) {
        auto keepalive = this->shared_from_this();
        (void)keepalive;
        if (co_await write_mutex_.lock(token) == coro::cancel_result::cancelled)
            co_return io::io_result{-ECANCELED, 0};
        sync::lock_guard guard(write_mutex_);
        {
            std::lock_guard lock(mutex);
            if (output.error()) co_return io::io_result{-output.error(), 0};
        }
        uint64_t observed;
        {
            std::lock_guard lock(mutex);
            observed = generation;
        }
        co_return co_await wait_change(observed, std::move(token));
    }

    coro::task<io::io_result> flush_to(uint64_t watermark, coro::cancel_token token) {
        auto keepalive = this->shared_from_this();
        (void)keepalive;
        start_output();
        for (;;) {
            uint64_t observed;
            {
                std::lock_guard lock(mutex);
                if (output.drained_bytes() >= watermark) co_return io::io_result{0, 0};
                if (output.error()) co_return io::io_result{-output.error(), 0};
                observed = generation;
            }
            auto result = co_await wait_change(observed, token);
            if (result.result < 0) {
                std::lock_guard lock(mutex);
                if (output.drained_bytes() >= watermark) co_return io::io_result{0, 0};
                if (output.error()) co_return io::io_result{-output.error(), 0};
                co_return result;
            }
        }
    }

    void fail(int error) noexcept {
        std::shared_ptr<coro::cancel_source> reader;
        {
            std::lock_guard lock(mutex);
            output.fail(error);
            reader = read_poll_;
        }
        cancel_noexcept(reader);
        // Do not close/reuse a descriptor while an operation owns it.
        if constexpr (requires(Lower& stream) { { stream.shutdown_socket() } noexcept; }) {
            lower.shutdown_socket();
        }
        try { pump_cancel_.cancel(); } catch (...) {}
        notify_progress();
    }

    class output_settlement {
    public:
        explicit output_settlement(std::shared_ptr<self_type> owner) noexcept
            : owner_(std::move(owner)) {}
        output_settlement(output_settlement&&) noexcept = default;
        output_settlement(const output_settlement&) = delete;
        ~output_settlement() { if (wake_) wake_->abandon(); }
        bool await_ready() const noexcept {
            std::lock_guard lock(owner_->mutex);
            return !owner_->pump_active_;
        }
        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            {
                std::lock_guard lock(owner_->mutex);
                if (!owner_->pump_active_) return false;
                // Settlement is terminal (failure or whole-session retirement).
                // Reader, writer and the one concurrent abort each need a slot;
                // no successor pump may start after terminal settlement begins.
                assert(owner_->operation_error());
                for (auto& slot : owner_->cleanup_slots_) {
                    if (slot.used) continue;
                    slot.used = true;
                    slot.registered = true;
                    wake_ = wake_ptr(owner_, &slot.wake); // alias, no allocation
                    slot.wake.set_handle_blocked(handle);
                    break;
                }
                assert(wake_ && "TLS operation concurrency contract violated");
                if (!wake_) std::terminate();
            }
            auto wake = wake_;
            return wake->unblock_after_publish();
        }
        void await_resume() const noexcept {}
    private:
        std::shared_ptr<self_type> owner_;
        wake_ptr wake_;
    };

    output_settlement settle_output() noexcept {
        return output_settlement(this->shared_from_this());
    }

private:
    static void cancel_noexcept(const std::shared_ptr<coro::cancel_source>& source) noexcept {
        if (source) { try { source->cancel(); } catch (...) {} }
    }

    static coro::task<void> pump(std::shared_ptr<self_type> self,
                               std::shared_ptr<launch_ticket> ticket) {
        ticket->entered.store(true, std::memory_order_release);
        ticket.reset();
        try {
            for (;;) {
                if (co_await self->write_mutex_.lock(self->pump_cancel_.get_token()) ==
                    coro::cancel_result::cancelled) {
                    self->fail(ECANCELED);
                    break;
                }
                sync::lock_guard write_guard(self->write_mutex_);
                std::span<const std::byte> bytes;
                bool finished = false;
#ifdef ELIO_RUNTIME_TEST_HOOKS
                void* inactive_context = nullptr;
                coro::task<void> (*inactive_hook)(void*, uint64_t) = nullptr;
                uint64_t inactive_drained = 0;
#endif
                {
                    std::lock_guard lock(self->mutex);
                    bytes = self->output.pending();
                    if (bytes.empty()) {
                        self->pump_active_ = false;
                        finished = true;
#ifdef ELIO_RUNTIME_TEST_HOOKS
                        inactive_context = self->output_progress_context;
                        inactive_hook = self->after_output_inactive;
                        inactive_drained = self->output.drained_bytes();
#endif
                    }
                }
                if (finished) {
                    self->notify_progress();
#ifdef ELIO_RUNTIME_TEST_HOOKS
                    if (inactive_hook) co_await inactive_hook(inactive_context, inactive_drained);
#endif
                    co_return;
                }
                // Keep the head allocation until completion cleanup, including
                // a late positive completion after terminal cancellation.
                auto sent = co_await self->lower.write(bytes.data(), bytes.size(),
                    self->pump_cancel_.get_token());
                if (sent.result > 0) {
#ifdef ELIO_RUNTIME_TEST_HOOKS
                    void* progress_context;
                    coro::task<void> (*progress_hook)(void*, uint64_t);
                    uint64_t drained;
#endif
                    {
                        std::lock_guard lock(self->mutex);
                        self->output.consume(static_cast<size_t>(sent.result));
#ifdef ELIO_RUNTIME_TEST_HOOKS
                        progress_context = self->output_progress_context;
                        progress_hook = self->after_output_progress;
                        drained = self->output.drained_bytes();
#endif
                    }
                    self->notify_progress();
#ifdef ELIO_RUNTIME_TEST_HOOKS
                    if (progress_hook) co_await progress_hook(progress_context, drained);
#endif
                    continue;
                }
                if (sent.result == -EINTR) continue;
                self->fail(sent.result < 0 ? -sent.result : EPIPE);
            }
        } catch (const std::bad_alloc&) {
            self->fail(ENOMEM);
        } catch (...) {
            self->fail(EIO);
        }
        {
            std::lock_guard lock(self->mutex);
            self->pump_active_ = false;
        }
        self->notify_progress();
    }

    sync::mutex read_mutex_;
    sync::mutex write_mutex_;
    wake_list waiters_;
    std::shared_ptr<coro::cancel_source> read_poll_;
    coro::cancel_source pump_cancel_;
    bool pump_active_ = false;
    bool output_retired_ = false;
    struct cleanup_slot {
        sync::detail::wake_state wake;
        bool used = false;
        bool registered = false;
    };
    std::array<cleanup_slot, cleanup_capacity> cleanup_slots_;
};

using tls_transport = basic_tls_transport<net::tcp_stream>;

} // namespace elio::tls::detail
