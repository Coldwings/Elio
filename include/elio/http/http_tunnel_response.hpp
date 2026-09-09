#pragma once
#include <elio/http/http_response_head.hpp>
#include <elio/http/http_tunnel.hpp>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace elio::http {
namespace detail { struct tunnel_response_access; }

class tunnel_response : public response_head {
public:
    template<typename Session>
        requires std::is_invocable_r_v<coro::task<tunnel_result>, std::decay_t<Session>&,
                                      tunnel_stream&, coro::cancel_token>
    tunnel_response(response_head head, Session&& session)
        : response_head(std::move(head)), session_(std::make_unique<model<std::decay_t<Session>>>(
              std::forward<Session>(session))) {}
    template<typename Session>
        requires std::is_invocable_r_v<coro::task<tunnel_result>, std::decay_t<Session>&,
                                      tunnel_stream&, coro::cancel_token>
    tunnel_response(status code, Session&& session)
        : tunnel_response(response_head(code), std::forward<Session>(session)) {}
    template<typename Session>
        requires std::is_invocable_r_v<coro::task<tunnel_result>, std::decay_t<Session>&,
                                      tunnel_stream&, coro::cancel_token>
    explicit tunnel_response(Session&& session)
        : tunnel_response(status::ok, std::forward<Session>(session)) {}
    tunnel_response(const tunnel_response&) = delete;
    tunnel_response& operator=(const tunnel_response&) = delete;
    tunnel_response(tunnel_response&&) noexcept = default;
    tunnel_response& operator=(tunnel_response&&) noexcept = default;
private:
    friend struct detail::tunnel_response_access;
    struct base {
        virtual ~base() = default;
        virtual coro::task<tunnel_result> run(tunnel_stream&, coro::cancel_token) = 0;
    };
    template<typename Session> struct model final : base {
        template<typename T> explicit model(T&& s) : session(std::forward<T>(s)) {}
        coro::task<tunnel_result> run(tunnel_stream& stream, coro::cancel_token token) override {
            co_return co_await std::invoke(session, stream, std::move(token));
        }
        Session session;
    };
    std::unique_ptr<base> session_;
    bool invoked_ = false;
};

namespace detail {
struct tunnel_response_access {
    static bool can_run(const tunnel_response& response) noexcept {
        return response.session_ && !response.invoked_;
    }
    static coro::task<tunnel_result> run(tunnel_response& response, tunnel_stream& stream,
                                        coro::cancel_token token) {
        if (!can_run(response)) co_return tunnel_result{tunnel_end::invalid_state, EINVAL};
        response.invoked_ = true;
        auto owned = std::move(response.session_);
        try { co_return co_await owned->run(stream, std::move(token)); }
        catch (const std::bad_alloc&) { co_return tunnel_result{tunnel_end::callback_error, ENOMEM}; }
        catch (...) { co_return tunnel_result{tunnel_end::callback_error, EIO}; }
    }
};
}
} // namespace elio::http
