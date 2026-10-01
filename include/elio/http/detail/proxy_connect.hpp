#pragma once

#include <elio/http/client_result.hpp>
#include <elio/http/detail/proxy_profile.hpp>
#include <elio/http/http_message.hpp>
#include <elio/http/http_response_reader.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <vector>

namespace elio::http::detail {

#ifdef ELIO_RUNTIME_TEST_HOOKS
enum class proxy_connect_step { writing, written, reading, received };
inline std::atomic<void (*)(proxy_connect_step, size_t, int,
    std::optional<std::chrono::steady_clock::time_point>)> proxy_connect_progress_for_test{nullptr};
using proxy_write_wait_hook = coro::task<void> (*)(coro::cancel_token);
inline std::atomic<proxy_write_wait_hook> proxy_connect_write_wait_for_test{nullptr};
#endif

// The connector owns stream/profile and its whole-setup watchdog through this
// await. The helper neither pools a rejected channel nor restarts the budget.
template<typename Stream>
coro::task<client_result<std::vector<char>>> negotiate_connect(Stream& stream,
        const route_endpoint& target, const proxy_profile& proxy, coro::cancel_token token = {},
        std::optional<std::chrono::steady_clock::time_point> deadline = {}) {
    const auto stopped = [&]() -> std::optional<client_error> {
        if (token.is_cancelled()) return make_client_error(ECANCELED, client_stage::proxy_connect);
        if (deadline && *deadline <= std::chrono::steady_clock::now())
            return make_client_error(ETIMEDOUT, client_stage::proxy_connect);
        return {};
    };
    if (auto error = stopped()) co_return *error;
    request setup(method::CONNECT, target.authority());
    setup.set_host(target.authority());
    setup.set_header("Connection", "keep-alive");
    if (!proxy.authorization.empty()) setup.set_header("Proxy-Authorization", proxy.authorization);
    const auto bytes = setup.serialize_headers();
    constexpr size_t max_request_bytes = 64 * 1024;
    if (bytes.size() > max_request_bytes)
        co_return make_client_error(EMSGSIZE, client_stage::proxy_connect);
    size_t offset = 0;
    while (offset < bytes.size()) {
        if (auto error = stopped()) co_return *error;
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = proxy_connect_progress_for_test.load(std::memory_order_acquire))
            hook(proxy_connect_step::writing, offset, 0, deadline);
        if (auto hook = proxy_connect_write_wait_for_test.load(std::memory_order_acquire)) {
            co_await hook(token);
            if (auto error = stopped()) co_return *error;
        }
#endif
        auto written = co_await stream.write(bytes.data() + offset, bytes.size() - offset, token);
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = proxy_connect_progress_for_test.load(std::memory_order_acquire))
            hook(proxy_connect_step::written, offset + (written.result > 0 ?
                static_cast<size_t>(written.result) : 0), written.result < 0 ? -written.result : 0,
                deadline);
#endif
        if (written.result == -EINTR) continue;
        if (written.result <= 0)
            co_return make_client_error(written.result < 0 ? -written.result : EIO,
                                       client_stage::proxy_connect);
        if (static_cast<size_t>(written.result) > bytes.size() - offset)
            co_return make_client_error(EOVERFLOW, client_stage::proxy_connect);
        offset += static_cast<size_t>(written.result);
    }

    response_reader reader(std::max(size_t{1}, std::min(size_t{8192}, proxy.limits.max_read_ahead)));
    reader.set_max_headers(proxy.limits.max_headers);
    reader.set_max_header_size(proxy.limits.max_header_size);
    reader.set_request_method(method::CONNECT);
    size_t received_bytes = 0;
    size_t interims = 0;
    auto receive = [&](void* data, size_t size) -> coro::task<io::io_result> {
        if (auto error = stopped()) co_return io::io_result{-error->code.value(), 0};
        if (received_bytes >= proxy.limits.max_response_bytes)
            co_return io::io_result{-EMSGSIZE, 0};
        size = std::min(size, proxy.limits.max_response_bytes - received_bytes);
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = proxy_connect_progress_for_test.load(std::memory_order_acquire))
            hook(proxy_connect_step::reading, received_bytes, 0, deadline);
#endif
        auto received = co_await stream.read(data, size, token);
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = proxy_connect_progress_for_test.load(std::memory_order_acquire))
            hook(proxy_connect_step::received, received_bytes + (received.result > 0 ?
                static_cast<size_t>(received.result) : 0), received.result < 0 ? -received.result : 0,
                deadline);
#endif
        if (received.result > 0) {
            if (static_cast<size_t>(received.result) > size)
                co_return io::io_result{-EOVERFLOW, 0};
            received_bytes += static_cast<size_t>(received.result);
        }
        co_return received;
    };
    for (;;) {
        if (auto error = stopped()) co_return *error;
        auto next = co_await reader.read_with(receive, token);
        if (next.error) co_return make_client_error(next.error, client_stage::proxy_connect);
        const auto code = reader.decoder().status_code();
        if (next.event == response_event::headers_complete) {
            if (code < 100 || code > 599)
                co_return make_client_error(EBADMSG, client_stage::proxy_connect);
            if (code == 101) co_return make_client_error(ENOTSUP, client_stage::proxy_connect);
            if (code >= 300)
                co_return make_client_error(code == 407 ? EACCES : ECONNREFUSED,
                                           client_stage::proxy_connect);
            if (code < 200) {
                if (interims >= proxy.limits.max_informational_responses)
                    co_return make_client_error(EMSGSIZE, client_stage::proxy_connect);
                ++interims;
            }
        } else if (next.event == response_event::protocol_handoff) {
            if (code < 200 || code >= 300)
                co_return make_client_error(ENOTSUP, client_stage::proxy_connect);
            const auto prefix = reader.remaining();
            if (prefix.size() > proxy.limits.max_read_ahead)
                co_return make_client_error(EMSGSIZE, client_stage::proxy_connect);
            co_return std::vector<char>(prefix.begin(), prefix.end());
        } else if (next.event == response_event::message_complete) {
            if (code >= 200 || !reader.next_response())
                co_return make_client_error(EBADMSG, client_stage::proxy_connect);
            reader.set_request_method(method::CONNECT);
        }
    }
}

} // namespace elio::http::detail
