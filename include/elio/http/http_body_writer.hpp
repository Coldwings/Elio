#pragma once

#include <elio/http/http_response_plan.hpp>
#include <elio/http/http_send_result.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/coro/task.hpp>
#include <elio/io/io_backend.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <span>
#include <sys/uio.h>

namespace elio::http {

/// Both descriptors and payload remain immutable and alive until writev returns.
struct body_buffer {
    const void* data = nullptr;
    size_t size = 0;
};

namespace detail {
struct body_writer_access;
#ifdef ELIO_RUNTIME_TEST_HOOKS
inline thread_local std::function<void()>* next_body_write_timeout_for_test = nullptr;
inline void capture_next_body_write_timeout_for_test(std::function<void()>& expire) {
    next_body_write_timeout_for_test = &expire;
}
inline thread_local std::function<void()>* next_body_write_internal_error_for_test = nullptr;
inline void capture_next_body_write_internal_error_for_test(std::function<void()>& fail) {
    next_body_write_internal_error_for_test = &fail;
}
enum class body_write_allocation_site { none, single_frame, operation_frame, arbitration, session_registration, operation_registration };
inline thread_local body_write_allocation_site fail_body_write_allocation_for_test = body_write_allocation_site::none;
inline void body_write_allocation_checkpoint_for_test(body_write_allocation_site site) {
    if (fail_body_write_allocation_for_test == site) {
        fail_body_write_allocation_for_test = body_write_allocation_site::none;
        throw std::bad_alloc();
    }
}
#endif
}

/// Server-scoped, sequential borrowed writes. No payload buffering or replay.
/// Cancellation is cooperative: return waits for transport and timer cleanup.
/// Coroutine-frame allocation may throw before a task can be returned. Such a
/// failure still terminates the writer; catching it cannot permit finalization.
class body_writer {
public:
    body_writer(const body_writer&) = delete;
    body_writer& operator=(const body_writer&) = delete;
    body_writer(body_writer&&) = delete;
    body_writer& operator=(body_writer&&) = delete;

    coro::task<send_result> write(std::string_view bytes, coro::cancel_token token = {}) {
        return start_one(bytes, operation::body, std::move(token));
    }

    coro::task<send_result> writev(std::span<const body_buffer> buffers,
                                  coro::cancel_token token = {}) {
        return start_operation(buffers, operation::body, std::move(token));
    }

private:
    friend struct detail::body_writer_access;
    using transport_fn = coro::task<io::io_result> (*)(void*, iovec*, size_t, coro::cancel_token);
    enum class phase { headers, open, finished, failed };
    enum class operation { headers, body, finish };
    enum class winner { pending, success, cancelled, timed_out, error };
    struct arbitration {
        std::atomic<winner> outcome{winner::pending};
        std::atomic<int> failure_errno{EIO};
        coro::cancel_source stop_io;

        bool claim(winner value) noexcept {
            auto pending = winner::pending;
            return outcome.compare_exchange_strong(pending, value, std::memory_order_acq_rel);
        }
        void interrupt(winner value) {
            if (claim(value)) stop_io.cancel();
        }
        void watchdog_failed(int error) {
            failure_errno.store(error, std::memory_order_relaxed);
            interrupt(winner::error);
        }
    };

    body_writer(void* stream, transport_fn transport, const response_plan& plan,
                coro::cancel_token token, std::chrono::nanoseconds timeout)
        : stream_(stream), transport_(transport), framing_(plan.framing),
          expected_(plan.expected_body_bytes), session_token_(std::move(token)), timeout_(timeout) {
        if (!plan.success()) fail({send_errc::invalid_response, plan.error});
    }

    send_result fail(send_result failure) noexcept {
        if (phase_ != phase::failed) {
            failure.confirmed_body_bytes = confirmed_;
            result_ = failure;
            phase_ = phase::failed;
        }
        return result_;
    }

    // These entry points intentionally are not coroutines: they cover frame
    // allocation before execution reaches perform's exception boundary.
    coro::task<send_result> start_operation(std::span<const body_buffer> buffers,
                                           operation op, coro::cancel_token token) {
        try {
#ifdef ELIO_RUNTIME_TEST_HOOKS
            detail::body_write_allocation_checkpoint_for_test(detail::body_write_allocation_site::operation_frame);
#endif
            return perform(buffers, op, std::move(token));
        } catch (const std::bad_alloc&) {
            fail({send_errc::transport_error, ENOMEM});
            throw;
        } catch (...) {
            fail({send_errc::transport_error, EIO});
            throw;
        }
    }

    coro::task<send_result> start_one(std::string_view bytes, operation op, coro::cancel_token token) {
        try {
#ifdef ELIO_RUNTIME_TEST_HOOKS
            detail::body_write_allocation_checkpoint_for_test(detail::body_write_allocation_site::single_frame);
#endif
            return perform_one(bytes, op, std::move(token));
        } catch (const std::bad_alloc&) {
            fail({send_errc::transport_error, ENOMEM});
            throw;
        } catch (...) {
            fail({send_errc::transport_error, EIO});
            throw;
        }
    }

    coro::task<send_result> perform_one(std::string_view bytes, operation op, coro::cancel_token token) {
        const body_buffer buffer{bytes.data(), bytes.size()};
        co_return co_await start_operation(std::span<const body_buffer>(&buffer, 1), op, std::move(token));
    }

    coro::task<send_result> perform(std::span<const body_buffer> buffers,
                                    operation op, coro::cancel_token token) {
        // Concurrent use violates the sequential writer contract. Do not
        // mutate state owned by the operation still awaiting cleanup.
        if (busy_.exchange(true, std::memory_order_acquire)) {
            co_return send_result{send_errc::invalid_state};
        }
        struct release_busy {
            std::atomic<bool>& busy;
            ~release_busy() { busy.store(false, std::memory_order_release); }
        } release{busy_};
        if (phase_ == phase::failed) co_return result_;
        if ((op == operation::headers && phase_ != phase::headers) ||
            (op != operation::headers && phase_ != phase::open)) {
            co_return fail({send_errc::invalid_state});
        }
        if (session_token_.is_cancelled() || token.is_cancelled()) {
            co_return fail({send_errc::cancelled, ECANCELED});
        }
        uint64_t total = 0;
        for (const auto& buffer : buffers) {
            if (buffer.size && !buffer.data) co_return fail({send_errc::invalid_response, EFAULT});
            if (buffer.size > std::numeric_limits<uint64_t>::max() - total) {
                co_return fail({send_errc::invalid_response, EOVERFLOW});
            }
            total += buffer.size;
        }
        if (op == operation::body && expected_ && total > *expected_ - confirmed_) {
            co_return fail({send_errc::length_mismatch});
        }
        if (op == operation::body && total > std::numeric_limits<uint64_t>::max() - confirmed_) {
            co_return fail({send_errc::invalid_response, EOVERFLOW});
        }
        if (op == operation::finish && expected_ && confirmed_ != *expected_) {
            co_return fail({send_errc::length_mismatch});
        }

        std::shared_ptr<arbitration> state;
        coro::cancel_registration session_registration;
        coro::cancel_registration operation_registration;
        std::optional<coro::cancel_source> stop_watchdog;
        std::optional<coro::join_handle<void>> watchdog;
        const bool timed = total != 0 && timeout_.count() > 0;
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = timed && timeout_ < std::chrono::steady_clock::time_point::max() - start
            ? start + timeout_ : std::chrono::steady_clock::time_point::max();
        int transport_error = 0;
        try {
#ifdef ELIO_RUNTIME_TEST_HOOKS
            detail::body_write_allocation_checkpoint_for_test(detail::body_write_allocation_site::arbitration);
#endif
            state = std::make_shared<arbitration>();
            session_registration = session_token_.on_cancel([state] { state->interrupt(winner::cancelled); });
#ifdef ELIO_RUNTIME_TEST_HOOKS
            detail::body_write_allocation_checkpoint_for_test(detail::body_write_allocation_site::session_registration);
#endif
            operation_registration = token.on_cancel([state] { state->interrupt(winner::cancelled); });
#ifdef ELIO_RUNTIME_TEST_HOOKS
            detail::body_write_allocation_checkpoint_for_test(detail::body_write_allocation_site::operation_registration);
#endif
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (auto* hook = std::exchange(detail::next_body_write_timeout_for_test, nullptr)) {
                *hook = [state] { state->interrupt(winner::timed_out); };
            }
            if (auto* hook = std::exchange(detail::next_body_write_internal_error_for_test, nullptr)) {
                *hook = [state] { state->watchdog_failed(ENOMEM); };
            }
#endif
            if (timed) {
                stop_watchdog.emplace();
                auto* scheduler = runtime::scheduler::current();
                if (!scheduler) {
                    if (state->claim(winner::error)) transport_error = ENOTSUP;
                } else {
                    watchdog.emplace(scheduler->go_joinable(
                        [state, deadline, stop = stop_watchdog->get_token()]() -> coro::task<void> {
                            try {
                                auto result = co_await time::sleep_for(deadline - std::chrono::steady_clock::now(), stop);
                                if (result == coro::cancel_result::completed) state->interrupt(winner::timed_out);
                            } catch (const std::bad_alloc&) {
                                state->watchdog_failed(ENOMEM);
                            } catch (...) {
                                state->watchdog_failed(EIO);
                            }
                        }));
                }
            }

            size_t cursor = 0;
            size_t offset = 0;
            uint64_t remaining = total;
            while (remaining && state->outcome.load(std::memory_order_acquire) == winner::pending) {
                if (timed && std::chrono::steady_clock::now() >= deadline) {
                    state->interrupt(winner::timed_out);
                    break;
                }
                std::array<iovec, 64> vectors{};
                std::array<bool, 64> payload{};
                std::array<char, 32> chunk_prefix{};
                const bool chunked = op == operation::body && framing_ == response_framing::chunked;
                size_t count = chunked ? 1 : 0;
                uint64_t batch = 0;
                // Bound vector metadata and wire chunks, independently of the
                // caller's descriptor count or total logical write size.
                constexpr uint64_t max_batch = uint64_t{1} << 30;
                while (cursor < buffers.size() && count < (chunked ? 63u : 64u) && batch < max_batch) {
                    const auto& buffer = buffers[cursor];
                    if (offset == buffer.size) { ++cursor; offset = 0; continue; }
                    const size_t size = static_cast<size_t>(std::min<uint64_t>(buffer.size - offset, max_batch - batch));
                    vectors[count] = {const_cast<char*>(static_cast<const char*>(buffer.data) + offset), size};
                    payload[count++] = op == operation::body;
                    offset += size;
                    batch += size;
                }
                if (chunked) {
                    auto converted = std::to_chars(chunk_prefix.data(), chunk_prefix.data() + 28, batch, 16);
                    *converted.ptr++ = '\r';
                    *converted.ptr++ = '\n';
                    vectors[0] = {chunk_prefix.data(), static_cast<size_t>(converted.ptr - chunk_prefix.data())};
                    vectors[count++] = {const_cast<char*>("\r\n"), 2};
                }
                size_t first = 0;
                while (first < count && state->outcome.load(std::memory_order_acquire) == winner::pending) {
                    if (timed && std::chrono::steady_clock::now() >= deadline) {
                        state->interrupt(winner::timed_out);
                        break;
                    }
                    auto sent = co_await transport_(stream_, vectors.data() + first, count - first, state->stop_io.get_token());
                    if (sent.result < 0) {
                        if (sent.result == -EINTR) continue;
                        // Readiness-aware transports must consume EAGAIN
                        // internally. Retrying it here would busy-poll.
                        if (state->claim(winner::error)) transport_error = sent.error_code();
                        break;
                    }
                    if (sent.result == 0) {
                        if (state->claim(winner::error)) transport_error = EPIPE;
                        break;
                    }
                    size_t progress = static_cast<size_t>(sent.result);
                    while (progress && first < count) {
                        const auto consumed = std::min(progress, vectors[first].iov_len);
                        if (payload[first]) confirmed_ += consumed;
                        vectors[first].iov_base = static_cast<char*>(vectors[first].iov_base) + consumed;
                        vectors[first].iov_len -= consumed;
                        progress -= consumed;
                        if (vectors[first].iov_len == 0) ++first;
                    }
                    if (progress) {
                        if (state->claim(winner::error)) transport_error = EIO;
                        break;
                    }
                }
                remaining -= batch;
            }
            if (timed && std::chrono::steady_clock::now() >= deadline) state->interrupt(winner::timed_out);
            state->claim(winner::success);
        } catch (const std::bad_alloc&) {
            if (!state || state->claim(winner::error)) transport_error = ENOMEM;
        } catch (...) {
            if (!state || state->claim(winner::error)) transport_error = EIO;
        }
        // Neither a winner nor cancellation authorizes asynchronous frame
        // destruction. Every transport await above completed before this join.
        if (stop_watchdog) stop_watchdog->cancel();
        if (watchdog) {
            try { co_await std::move(*watchdog); }
            catch (...) {
                // Preserve any terminal winner, including success; a late
                // watchdog cleanup failure cannot undo wire completion.
                if (state->claim(winner::error)) transport_error = EIO;
            }
        }
        session_registration = {};
        operation_registration = {};
        if (!state) co_return fail({send_errc::transport_error, transport_error});
        const auto outcome = state->outcome.load(std::memory_order_acquire);
        if (outcome == winner::cancelled) co_return fail({send_errc::cancelled, ECANCELED});
        if (outcome == winner::timed_out) co_return fail({send_errc::timed_out, ETIMEDOUT});
        if (outcome == winner::error) co_return fail({send_errc::transport_error,
            transport_error ? transport_error : state->failure_errno.load(std::memory_order_relaxed)});
        if (op == operation::headers) phase_ = phase::open;
        if (op == operation::finish) phase_ = phase::finished;
        result_.confirmed_body_bytes = confirmed_;
        co_return result_;
    }

    void* stream_;
    transport_fn transport_;
    response_framing framing_;
    std::optional<uint64_t> expected_;
    coro::cancel_token session_token_;
    std::chrono::nanoseconds timeout_;
    std::atomic<bool> busy_{false};
    phase phase_ = phase::headers;
    uint64_t confirmed_ = 0;
    send_result result_;
};

namespace detail {
struct body_writer_access {
    /// timeout <= 0 disables the per-logical-write timer, not cancellation.
    template<typename Stream>
    static body_writer create(Stream& stream, const response_plan& plan,
                              coro::cancel_token token = {},
                              std::chrono::nanoseconds timeout = {}) {
        return body_writer(&stream,
            [](void* erased, iovec* vectors, size_t count, coro::cancel_token stop) -> coro::task<io::io_result> {
                co_return co_await static_cast<Stream*>(erased)->writev(vectors, count, std::move(stop));
            }, plan, std::move(token), timeout);
    }
    static coro::task<send_result> send_headers(body_writer& writer, std::string_view headers,
                                               coro::cancel_token token = {}) {
        return writer.start_one(headers, body_writer::operation::headers, std::move(token));
    }
    static coro::task<send_result> finish(body_writer& writer) {
        const std::string_view ending = writer.framing_ == response_framing::chunked ? "0\r\n\r\n" : "";
        return writer.start_one(ending, body_writer::operation::finish, {});
    }
    static send_result fail(body_writer& writer, send_result error) noexcept { return writer.fail(error); }
    static send_result result(const body_writer& writer) noexcept { return writer.result_; }
};
}
} // namespace elio::http
