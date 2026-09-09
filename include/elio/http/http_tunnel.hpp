#pragma once

#include <elio/coro/task.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/io/io_backend.hpp>
#include <elio/net/stream_close.hpp>
#include <elio/http/http_common.hpp>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <sys/uio.h>
#include <utility>

namespace elio::http {

enum class tunnel_end { completed, session_closed, cancelled, timed_out,
    transport_error, invalid_response, invalid_state, callback_error };

struct tunnel_direction_result {
    uint64_t source_bytes = 0;
    uint64_t accepted_bytes = 0;
    bool uncertain_write = false;
};

struct tunnel_result {
    tunnel_end end = tunnel_end::completed;
    int error = 0;
    tunnel_direction_result client_to_upstream{};
    tunnel_direction_result upstream_to_client{};
    bool success() const noexcept {
        return end == tunnel_end::completed || end == tunnel_end::session_closed;
    }
};

struct tunnel_write_result {
    uint64_t accepted_bytes = 0;
    int error = 0;
    bool uncertain_attempt = false;
    bool success() const noexcept { return error == 0; }
};

namespace detail {
struct tunnel_stream_access;
enum class tunnel_frame { read, single_write, vector_write, finish };
}

// Callback-scoped view. The transport, caller buffers and descriptors remain
// borrowed until their operations return; only parser read-ahead is owned.
class tunnel_stream {
public:
    tunnel_stream(const tunnel_stream&) = delete;
    tunnel_stream& operator=(const tunnel_stream&) = delete;
    tunnel_stream(tunnel_stream&&) = delete;
    tunnel_stream& operator=(tunnel_stream&&) = delete;

    int error() const noexcept { return error_.load(std::memory_order_acquire); }

#ifdef ELIO_RUNTIME_TEST_HOOKS
    void set_frame_test_hook(void* context, void (*hook)(void*, detail::tunnel_frame)) noexcept {
        frame_context_ = context;
        frame_hook_ = hook;
    }
#endif

    // Failure before a task exists terminalizes the view, then rethrows.
    // Callers must still join any overlapping operation after catching it.
    coro::task<io::io_result> read(void* buffer, size_t length, coro::cancel_token token = {}) {
        return start_frame(detail::tunnel_frame::read,
            [&] { return read_impl(buffer, length, std::move(token)); });
    }
    net::close_scope read_end_scope() const { return functions_->read_scope(object_); }
    coro::task<tunnel_write_result> write(const void* data, size_t size, coro::cancel_token token = {}) {
        return start_frame(detail::tunnel_frame::single_write,
            [&] { return write_impl(data, size, std::move(token)); });
    }
    coro::task<tunnel_write_result> write(std::string_view data, coro::cancel_token token = {}) {
        return write(data.data(), data.size(), std::move(token));
    }
    coro::task<tunnel_write_result> writev(std::span<const iovec> parts, coro::cancel_token token = {}) {
        return start_frame(detail::tunnel_frame::vector_write,
            [&] { return writev_impl(parts, std::move(token)); });
    }
    coro::task<net::write_finish_result> finish_output(coro::cancel_token token = {}) {
        return start_frame(detail::tunnel_frame::finish,
            [&] { return finish_impl(std::move(token)); });
    }

private:
    template<class Factory>
    std::invoke_result_t<Factory> start_frame(detail::tunnel_frame site, Factory&& factory) {
        try {
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (frame_hook_) frame_hook_(frame_context_, site);
#else
            (void)site;
#endif
            return std::forward<Factory>(factory)();
        } catch (const std::bad_alloc&) { fail(ENOMEM); throw; }
        catch (...) { fail(EIO); throw; }
    }

    coro::task<io::io_result> read_impl(void* buffer, size_t length, coro::cancel_token token) {
        if (error()) co_return io::io_result{-error(), 0};
        if (!length) co_return io::io_result{0, 0};
        if (!buffer) { fail(EINVAL); co_return io::io_result{-EINVAL, 0}; }
        int failure = EIO;
        try {
            auto registration = token.on_cancel([this] { fail(ECANCELED); });
            if (error()) co_return io::io_result{-error(), 0};
            const auto count = std::min({length, prefix_.size() - prefix_offset_, size_t{INT32_MAX}});
            if (count) {
                std::memcpy(buffer, prefix_.data() + prefix_offset_, count);
                prefix_offset_ += count;
                co_return io::io_result{static_cast<int32_t>(count), 0};
            }
            auto result = co_await functions_->read(object_, buffer, std::min(length, size_t{INT32_MAX}), stop_.get_token());
            if (result.result > 0 && static_cast<size_t>(result.result) > std::min(length, size_t{INT32_MAX})) {
                fail(EIO); co_return io::io_result{-EIO, 0};
            }
            if (result.result < 0) {
                fail(-result.result);
                // A sibling failure may have requested this cancellation.
                // Preserve that primary cause instead of its cleanup symptom.
                result.result = -error();
            }
            co_return result;
        } catch (const std::bad_alloc&) { failure = ENOMEM; }
        catch (...) {}
        fail(failure);
        co_return io::io_result{-error(), 0};
    }

    coro::task<tunnel_write_result> write_impl(const void* data, size_t size,
                                              coro::cancel_token token) {
        iovec part{const_cast<void*>(data), size};
        try {
            co_return co_await writev(std::span<const iovec>(&part, 1), std::move(token));
        } catch (const std::bad_alloc&) { fail(ENOMEM); }
        catch (...) { fail(EIO); }
        co_return tunnel_write_result{0, error(), false};
    }

    coro::task<tunnel_write_result> writev_impl(std::span<const iovec> parts,
                                               coro::cancel_token token) {
        tunnel_write_result result;
        if (error()) { result.error = error(); co_return result; }
        if (output_finished_.load(std::memory_order_acquire)) {
            result.error = ESHUTDOWN; co_return result;
        }
        int failure = EIO;
        try {
            uint64_t total = 0;
            for (const auto& part : parts) {
                if ((!part.iov_base && part.iov_len) || part.iov_len > UINT64_MAX - total) {
                    fail(!part.iov_base && part.iov_len ? EINVAL : EOVERFLOW);
                    result.error = error(); co_return result;
                }
                total += part.iov_len;
            }
            auto registration = token.on_cancel([this] { fail(ECANCELED); });
            for (const auto& part : parts) {
                size_t offset = 0;
                while (offset < part.iov_len) {
                    if (error()) { result.error = error(); co_return result; }
                    const auto size = std::min(part.iov_len - offset, size_t{INT32_MAX});
                    result.uncertain_attempt = true;
                    auto written = co_await functions_->write(object_,
                        static_cast<const char*>(part.iov_base) + offset, size, stop_.get_token());
                    if (written.result == -ESHUTDOWN &&
                        read_end_scope() == net::close_scope::whole_session) {
                        // The peer's close driver can still be flushing its
                        // committed prefix/alert. Do not cancel that reader.
                        output_finished_.store(true, std::memory_order_release);
                        result.error = ESHUTDOWN;
                        co_return result;
                    }
                    if (written.result <= 0 || static_cast<size_t>(written.result) > size) {
                        fail(written.result < 0 ? -written.result : EIO);
                        result.error = error(); co_return result;
                    }
                    offset += static_cast<size_t>(written.result);
                    result.accepted_bytes += static_cast<uint64_t>(written.result);
                    result.uncertain_attempt = false;
                }
            }
            result.error = error();
            co_return result;
        } catch (const std::bad_alloc&) { failure = ENOMEM; }
        catch (...) {}
        fail(failure);
        result.error = error();
        co_return result;
    }

    coro::task<net::write_finish_result> finish_impl(coro::cancel_token token) {
        if (error()) co_return net::write_finish_result{net::close_scope::whole_session, error()};
        int failure = EIO;
        try {
            auto registration = token.on_cancel([this] { fail(ECANCELED); });
            if (error()) co_return net::write_finish_result{net::close_scope::whole_session, error()};
            auto result = co_await functions_->finish(object_, stop_.get_token());
            if (result.error) {
                fail(result.error);
                result.error = error();
            }
            else output_finished_.store(true, std::memory_order_release);
            co_return result;
        } catch (const std::bad_alloc&) { failure = ENOMEM; }
        catch (...) {}
        fail(failure);
        co_return net::write_finish_result{net::close_scope::whole_session, error()};
    }

private:
    friend struct detail::tunnel_stream_access;
    struct functions {
        coro::task<io::io_result> (*read)(void*, void*, size_t, coro::cancel_token);
        coro::task<io::io_result> (*write)(void*, const void*, size_t, coro::cancel_token);
        coro::task<net::write_finish_result> (*finish)(void*, coro::cancel_token);
        net::close_scope (*read_scope)(const void*);
    };
    template<typename Stream>
    tunnel_stream(Stream& stream, std::string prefix)
        : object_(&stream), functions_(&table<Stream>), prefix_(std::move(prefix)) {}
    void fail(int value) noexcept {
        int expected = 0;
        error_.compare_exchange_strong(expected, value > 0 ? value : EIO, std::memory_order_acq_rel);
        try { stop_.cancel(); } catch (...) {}
    }
    template<typename Stream>
    static inline const functions table{
        +[](void* p, void* b, size_t n, coro::cancel_token t) { return static_cast<Stream*>(p)->read(b, n, std::move(t)); },
        +[](void* p, const void* b, size_t n, coro::cancel_token t) { return static_cast<Stream*>(p)->write(b, n, std::move(t)); },
        +[](void* p, coro::cancel_token t) { return static_cast<Stream*>(p)->finish_write(std::move(t)); },
        +[](const void* p) { return static_cast<const Stream*>(p)->read_end_scope(); }
    };
    void* object_;
    const functions* functions_;
    std::string prefix_;
    size_t prefix_offset_ = 0;
    std::atomic<int> error_{0};
    std::atomic<bool> output_finished_{false};
    coro::cancel_source stop_;
#ifdef ELIO_RUNTIME_TEST_HOOKS
    void* frame_context_ = nullptr;
    void (*frame_hook_)(void*, detail::tunnel_frame) = nullptr;
#endif
};

namespace detail {
struct tunnel_stream_access {
    template<typename Stream>
    static tunnel_stream create(Stream& stream, std::string prefix = {}) {
        return tunnel_stream(stream, std::move(prefix));
    }
};
}
} // namespace elio::http
