#pragma once

#include "output_bio.hpp"
#include "../../net/tcp.hpp"
#include "../../sync/mutex.hpp"
#include "../../sync/detail/wake_state.hpp"
#include "../../runtime/scheduler.hpp"

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace elio::tls::detail {

// Owns ciphertext and the socket, never SSL or caller plaintext. The TLS owner
// must call fail() on abandonment: an active pump deliberately retains this
// object until the kernel has released its borrowed ciphertext lease.
class tls_transport : public std::enable_shared_from_this<tls_transport> {
    using wake_ptr = sync::detail::wake_state_ptr;
    using wake_list = std::list<wake_ptr>;

    struct launch_ticket {
        std::shared_ptr<tls_transport> owner;
        bool entered = false;
        ~launch_ticket() {
            if (entered) return;
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
        change_waiter(std::shared_ptr<tls_transport> owner, uint64_t observed,
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
        std::shared_ptr<tls_transport> owner_;
        uint64_t observed_;
        wake_ptr wake_;
        wake_list pending_;
        coro::cancel_token::registration registration_;
    };

public:
    tls_transport(net::tcp_stream stream, size_t budget)
        : tcp(std::move(stream)), output(tcp.fd(), budget) {}

    net::tcp_stream tcp;
    output_bio_state output;
    std::mutex mutex;
    // Read or modify only under mutex. All methods below acquire mutex and
    // therefore must be called outside the SSL/state critical section.
    uint64_t generation = 0;
#ifdef ELIO_RUNTIME_TEST_HOOKS
    void* read_publish_context = nullptr;
    void (*before_read_publish)(void*) = nullptr;
    bool output_active_for_test() const noexcept { return pump_active_; }
#endif

    void notify_progress() noexcept {
        wake_list ready;
        std::shared_ptr<coro::cancel_source> reader;
        {
            std::lock_guard lock(mutex);
            ++generation;
            reader = read_poll_;
            ready.splice(ready.end(), waiters_);
            for (const auto& wake : ready) wake->claim_notification();
        }
        cancel_noexcept(reader);
        for (const auto& wake : ready) wake->schedule_claimed();
    }

    coro::task<io::io_result> wait_change(uint64_t observed, coro::cancel_token token) {
        co_return co_await change_waiter(shared_from_this(), observed, std::move(token));
    }

    coro::task<io::io_result> wait_read(uint64_t observed, coro::cancel_token token) {
        auto self = shared_from_this();
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
            read_poll_ = source;
        }
        io::io_result result;
        try {
            auto polled = co_await tcp.poll_read(source->get_token());
            result = polled.io;
        } catch (...) {
            std::lock_guard lock(mutex);
            if (read_poll_ == source) read_poll_.reset();
            throw;
        }
        {
            std::lock_guard lock(mutex);
            if (read_poll_ == source) read_poll_.reset();
            if (output.error()) result = {-output.error(), 0};
            else if (token.is_cancelled()) result = {-ECANCELED, 0};
            else if (source->is_cancelled()) result = {0, 0};
        }
        // The next logical reader must retry SSL before registering another
        // poll: this readiness may already have been consumed by its sibling.
        if (result.result >= 0) notify_progress();
        co_return result;
    }

    void start_output() noexcept {
        {
            std::lock_guard lock(mutex);
            if (pump_active_ || output.error() || !output.pending_bytes()) return;
            pump_active_ = true;
        }
        try {
            auto* scheduler = runtime::scheduler::current();
            if (!scheduler) throw std::runtime_error("TLS output requires a scheduler");
            auto self = shared_from_this();
            auto ticket = std::make_unique<launch_ticket>();
            ticket->owner = self;
            scheduler->go(pump(std::move(self), std::move(ticket)));
        } catch (...) {
            fail(ENOMEM);
            {
                std::lock_guard lock(mutex);
                pump_active_ = false;
            }
            notify_progress();
        }
    }

    coro::task<io::io_result> wait_write(coro::cancel_token token) {
        auto self = shared_from_this();
        if (co_await write_mutex_.lock(token) == coro::cancel_result::cancelled)
            co_return io::io_result{-ECANCELED, 0};
        sync::lock_guard guard(write_mutex_);
        {
            std::lock_guard lock(mutex);
            if (output.error()) co_return io::io_result{-output.error(), 0};
        }
        auto ready = co_await tcp.poll_write(token);
        co_return ready.was_cancelled() ? io::io_result{-ECANCELED, 0} : ready.io;
    }

    coro::task<io::io_result> flush_to(uint64_t watermark, coro::cancel_token token) {
        auto self = shared_from_this();
        start_output();
        for (;;) {
            uint64_t observed;
            {
                std::lock_guard lock(mutex);
                if (output.error()) co_return io::io_result{-output.error(), 0};
                if (output.drained_bytes() >= watermark) co_return io::io_result{0, 0};
                observed = generation;
            }
            auto result = co_await wait_change(observed, token);
            if (result.result < 0) co_return result;
        }
    }

    void fail(int error) noexcept {
        {
            std::lock_guard lock(mutex);
            output.fail(error);
        }
        // Do not close/reuse the descriptor while an operation owns it.
        if (tcp.fd() >= 0) ::shutdown(tcp.fd(), SHUT_RDWR);
        try { pump_cancel_.cancel(); } catch (...) {}
        notify_progress();
    }

    coro::task<void> settle_output() {
        auto self = shared_from_this();
        for (;;) {
            uint64_t observed;
            {
                std::lock_guard lock(mutex);
                if (!pump_active_) co_return;
                observed = generation;
            }
            // This cleanup wait intentionally ignores caller cancellation.
            (void)co_await wait_change(observed, {});
        }
    }

private:
    static void cancel_noexcept(const std::shared_ptr<coro::cancel_source>& source) noexcept {
        if (source) { try { source->cancel(); } catch (...) {} }
    }

    static coro::task<void> pump(std::shared_ptr<tls_transport> self,
                               std::unique_ptr<launch_ticket> ticket) {
        ticket->entered = true;
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
                {
                    std::lock_guard lock(self->mutex);
                    bytes = self->output.pending();
                    if (bytes.empty()) {
                        self->pump_active_ = false;
                        finished = true;
                    }
                }
                if (finished) {
                    self->notify_progress();
                    co_return;
                }
                // Keep the head allocation until completion cleanup, including
                // a late positive completion after terminal cancellation.
                auto sent = co_await io::async_send(self->tcp.fd(), bytes.data(),
                    bytes.size(), MSG_NOSIGNAL, self->pump_cancel_.get_token());
                if (sent.io.result > 0) {
                    {
                        std::lock_guard lock(self->mutex);
                        self->output.consume(static_cast<size_t>(sent.io.result));
                    }
                    self->notify_progress();
                    continue;
                }
                if (sent.io.result == -EINTR) continue;
                if (sent.io.result == -EAGAIN || sent.io.result == -EWOULDBLOCK) {
                    auto ready = co_await self->tcp.poll_write(self->pump_cancel_.get_token());
                    if (ready.io.result >= 0 && !ready.was_cancelled()) continue;
                    self->fail(ready.io.result < 0 ? -ready.io.result : ECANCELED);
                } else {
                    self->fail(sent.io.result < 0 ? -sent.io.result : EPIPE);
                }
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
};

} // namespace elio::tls::detail
