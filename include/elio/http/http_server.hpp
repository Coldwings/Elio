#pragma once

#include <elio/http/http_common.hpp>
#include <elio/http/http_parser.hpp>
#include <elio/http/http_message.hpp>
#include <elio/http/http_response_sender.hpp>
#include <elio/net/tcp.hpp>
#include <elio/tls/tls_stream.hpp>
#include <elio/io/io_context.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>
#include <elio/log/macros.hpp>

#include <sys/socket.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace elio::http {

namespace detail {

struct context_access;

struct tls_handshake_result {
    bool ok = false;
    bool timed_out = false;
};

inline coro::task<tls_handshake_result>
perform_tls_handshake_with_timeout(tls::tls_stream& stream,
                                   std::chrono::seconds timeout,
                                   coro::cancel_token token = {}) {
    auto* sched = runtime::scheduler::current();
    if (!sched || timeout.count() <= 0) {
        tls_handshake_result result;
        result.ok = co_await stream.handshake(token);
        co_return result;
    }

    auto timed_out = std::make_shared<std::atomic<bool>>(false);
    auto handshake_done = std::make_shared<std::atomic<bool>>(false);
    auto cancel_src = std::make_shared<coro::cancel_source>();
    auto forward = token.on_cancel([cancel_src] { cancel_src->cancel(); });
    auto* stream_ptr = &stream;

    auto watchdog = sched->go_joinable(
        [stream_ptr, timed_out, handshake_done, cancel_src,
         timeout]() -> coro::task<void> {
            auto r = co_await elio::time::sleep_for(timeout,
                                                    cancel_src->get_token());
            if (r == coro::cancel_result::completed &&
                !handshake_done->load(std::memory_order_acquire)) {
                timed_out->store(true, std::memory_order_release);
                cancel_src->cancel();
                stream_ptr->shutdown_socket();
            }
            co_return;
        });

    bool ok = co_await stream.handshake(cancel_src->get_token());
    handshake_done->store(true, std::memory_order_release);
    cancel_src->cancel();
    co_await std::move(watchdog);

    tls_handshake_result result;
    result.timed_out = timed_out->load(std::memory_order_acquire);
    result.ok = ok && !result.timed_out;
    co_return result;
}

} // namespace detail

/// HTTP request context passed to handlers
class context {
public:
    /// Type-erased writer used by send_interim(). The server installs one
    /// per request; it writes the serialized bytes to the connection.
    using interim_writer = std::function<coro::task<bool>(std::string_view)>;

    context(request req, std::string_view client_addr,
            interim_writer writer = {}, coro::cancel_token token = {})
        : request_(std::move(req)), client_addr_(client_addr),
          interim_writer_(std::move(writer)), token_(std::move(token)) {}

    coro::cancel_token cancel_token() const noexcept { return token_; }
    
    /// Get the request
    const request& req() const noexcept { return request_; }
    request& req() noexcept { return request_; }
    
    /// Get client address
    std::string_view client_addr() const noexcept { return client_addr_; }
    
    /// Get path parameter by name
    std::string_view param(std::string_view name) const {
        auto it = params_.find(std::string(name));
        if (it != params_.end()) {
            return it->second;
        }
        return {};
    }
    
    /// Set path parameter
    void set_param(std::string_view name, std::string_view value) {
        params_[std::string(name)] = std::string(value);
    }
    
    /// Get query parameter
    std::string query_param(std::string_view name) const {
        auto params = request_.query_params();
        auto it = params.find(std::string(name));
        if (it != params.end()) {
            return it->second;
        }
        return {};
    }
    
    /// Get all path parameters
    const std::unordered_map<std::string, std::string>& params() const noexcept {
        return params_;
    }

    /// Send an interim (1xx) response before the final response.
    ///
    /// `resp` must carry a 1xx status other than 101 Switching Protocols
    /// (a protocol upgrade is never an interim response); anything else
    /// sets `errno = EINVAL` and returns false. Multiple interim responses
    /// may be sent; RFC 9110 §15.2 permits any number of them. Body and
    /// framing headers are never serialized for 1xx, so e.g.
    /// `response(status::continue_)` writes exactly
    /// "HTTP/1.1 100 Continue\r\n\r\n".
    ///
    /// The context lives through the selected producer and its send cleanup;
    /// it must not escape that request scope (e.g. into a detached task), and
    /// interims can only be sent before the handler returns the final
    /// response. Contexts dispatched without a connection writer — e.g.
    /// the plain-HTTP fallback routes of `websocket::ws_server` — cannot
    /// send interims: send_interim() then sets `errno = ENOTSUP` and
    /// returns false, and the final response is unaffected.
    ///
    /// Note: the server reads the full request (including the body) before
    /// dispatching to the handler, so an explicit `100 Continue` sent here
    /// cannot accelerate an `Expect: 100-continue` client's first body
    /// send; it serves clients that pipeline or that wait for an interim
    /// the application wants to emit deliberately.
    coro::task<bool> send_interim(const response& resp) {
        if (final_selected_) {
            errno = EALREADY;
            co_return false;
        }
        const auto code = resp.status_code();
        if (code < 100 || code >= 200 ||
            code == static_cast<uint16_t>(status::switching_protocols)) {
            errno = EINVAL;
            co_return false;
        }
        if (!interim_writer_) {
            errno = ENOTSUP;
            co_return false;
        }
        co_return co_await interim_writer_(resp.serialize());
    }

private:
    friend struct detail::context_access;
    request request_;
    std::string client_addr_;
    std::unordered_map<std::string, std::string> params_;
    interim_writer interim_writer_;
    coro::cancel_token token_;
    bool final_selected_ = false;
};

/// Handler function type
using handler_func = std::function<coro::task<reply>(context&)>;

/// Synchronous handler function type
using sync_handler_func = std::function<response(context&)>;

namespace detail {
struct context_access {
    static void seal_final_response(context& ctx) noexcept { ctx.final_selected_ = true; }
};

template<typename Result>
concept handler_result = std::same_as<Result, response> || std::same_as<Result, streaming_response> ||
    std::same_as<Result, reply> || std::same_as<Result, coro::task<response>> ||
    std::same_as<Result, coro::task<streaming_response>> || std::same_as<Result, coro::task<reply>>;

template<typename Handler>
concept response_handler = std::copy_constructible<std::decay_t<Handler>> &&
    requires(std::decay_t<Handler>& handler, context& ctx) {
        requires handler_result<std::invoke_result_t<std::decay_t<Handler>&, context&>>;
        std::invoke(handler, ctx);
    };

template<response_handler Handler>
handler_func adapt_handler(Handler handler) {
    return [handler = std::move(handler)](context& ctx) mutable -> coro::task<reply> {
        using result_type = std::invoke_result_t<Handler&, context&>;
        if constexpr (std::same_as<result_type, response> || std::same_as<result_type, streaming_response> ||
                      std::same_as<result_type, reply>) {
            co_return reply{std::invoke(handler, ctx)};
        } else {
            co_return reply{co_await std::invoke(handler, ctx)};
        }
    };
}
} // namespace detail

/// Path segment kind for route matching
enum class segment_kind {
    literal,    ///< Match exact text
    param,      ///< Capture single non-empty path component into named parameter
    wildcard,   ///< Match zero or more remaining components (only meaningful as last segment)
};

/// Compiled path segment
struct route_segment {
    segment_kind kind;
    std::string value;  ///< literal text, or param name (without leading ':'); empty for wildcard
};

/// Route definition
struct route {
    method http_method;
    std::string pattern;
    std::vector<route_segment> segments;
    std::vector<std::string> param_names;  ///< Kept for backwards compatibility / introspection
    handler_func handler;

    /// Check if path matches and extract parameters
    bool match(std::string_view path, std::unordered_map<std::string, std::string>& params) const {
        // Tokenize `path` into components by splitting on '/'.
        // For example, "/foo/bar" -> ["", "foo", "bar"], "/foo/" -> ["", "foo", ""].
        // This makes trailing-slash handling consistent: components count differs from
        // patterns without the trailing slash, so they will not collide.
        std::vector<std::string_view> components;
        components.reserve(8);
        size_t start = 0;
        for (size_t i = 0; i <= path.size(); ++i) {
            if (i == path.size() || path[i] == '/') {
                components.emplace_back(path.data() + start, i - start);
                start = i + 1;
            }
        }

        size_t si = 0;  // segment index
        size_t ci = 0;  // component index
        while (si < segments.size()) {
            const auto& seg = segments[si];
            if (seg.kind == segment_kind::wildcard) {
                // Wildcard matches the remainder of the path (zero or more
                // components). The slash before the wildcard in the pattern
                // implies at least one trailing component must exist, mirroring
                // regex "^.../.*$": "/foo" does NOT match "/foo/*" (no
                // trailing slash), but "/foo/" matches because splitting
                // "/foo/" -> ["", "foo", ""] gives an empty trailing component
                // that the wildcard consumes (regex ".*" matches empty).
                return ci < components.size();
            }
            if (ci >= components.size()) return false;
            if (seg.kind == segment_kind::literal) {
                if (components[ci] != seg.value) return false;
            } else {
                // param: must match a non-empty component (mirrors regex "([^/]+)").
                if (components[ci].empty()) return false;
                params[seg.value] = std::string(components[ci]);
            }
            ++si;
            ++ci;
        }
        // No more segments to match — accept only if all path components consumed.
        return ci == components.size();
    }
};

/// HTTP router for path-based routing
class router {
public:
    router() = default;

    /// Add a route with async handler
    void add_route(method m, std::string_view pattern, handler_func handler) {
        route r;
        r.http_method = m;
        r.pattern = pattern;
        r.handler = std::move(handler);

        // Parse the pattern into a flat segment list. We split on '/' the same way
        // match() splits the request path, so empty leading/trailing segments are
        // preserved (this is what gives "/foo" vs "/foo/" their distinct identity).
        // Within a component:
        //   - ":name" => param segment (entire component must be ":name").
        //   - "*"     => wildcard segment; consumes the rest of the path.
        //   - anything else is treated as a literal (matched verbatim, no escaping).
        size_t start = 0;
        for (size_t i = 0; i <= pattern.size(); ++i) {
            if (i == pattern.size() || pattern[i] == '/') {
                std::string_view comp(pattern.data() + start, i - start);
                route_segment seg;
                if (!comp.empty() && comp.front() == ':') {
                    seg.kind = segment_kind::param;
                    seg.value.assign(comp.data() + 1, comp.size() - 1);
                    r.param_names.push_back(seg.value);
                } else if (comp == "*") {
                    if (i != pattern.size()) {
                        throw std::invalid_argument(
                            "HTTP route wildcard must be the final path segment");
                    }
                    seg.kind = segment_kind::wildcard;
                } else {
                    seg.kind = segment_kind::literal;
                    seg.value.assign(comp.data(), comp.size());
                }
                r.segments.push_back(std::move(seg));
                start = i + 1;
            }
        }

        routes_.push_back(std::move(r));
    }
    
    /// Normalize explicitly supported synchronous and asynchronous reply types.
    template<detail::response_handler Handler>
    void add_route(method m, std::string_view pattern, Handler handler) {
        add_route(m, pattern, detail::adapt_handler(std::move(handler)));
    }

    template<detail::response_handler Handler>
    void get(std::string_view pattern, Handler handler) {
        add_route(method::GET, pattern, std::move(handler));
    }

    template<detail::response_handler Handler>
    void post(std::string_view pattern, Handler handler) {
        add_route(method::POST, pattern, std::move(handler));
    }

    template<detail::response_handler Handler>
    void put(std::string_view pattern, Handler handler) {
        add_route(method::PUT, pattern, std::move(handler));
    }

    template<detail::response_handler Handler>
    void del(std::string_view pattern, Handler handler) {
        add_route(method::DELETE_, pattern, std::move(handler));
    }

    template<detail::response_handler Handler>
    void patch(std::string_view pattern, Handler handler) {
        add_route(method::PATCH, pattern, std::move(handler));
    }

    template<detail::response_handler Handler>
    void options(std::string_view pattern, Handler handler) {
        add_route(method::OPTIONS, pattern, std::move(handler));
    }

    /// Find matching route for request
    const route* find_route(method m, std::string_view path, 
                           std::unordered_map<std::string, std::string>& params) const {
        for (const auto& r : routes_) {
            if (r.http_method == m && r.match(path, params)) {
                return &r;
            }
        }
        return nullptr;
    }
    
private:
    std::vector<route> routes_;
};

/// HTTP server configuration
struct server_config {
    /// Max aggregate HTTP request bytes: request line, headers, and body.
    /// For WebSocket upgrades, bytes after the completed HTTP upgrade request
    /// belong to the WebSocket stream and are not counted here.
    size_t max_request_size = 10 * 1024 * 1024;
    size_t read_buffer_size = 8192;               ///< Read buffer size
    std::chrono::seconds keep_alive_timeout{30};  ///< Request and inbound TLS handshake timeout
    size_t max_keep_alive_requests = 100;         ///< Max requests per connection
    bool enable_logging = true;                   ///< Log requests
    std::chrono::milliseconds write_timeout{0};    ///< Per logical write; <= 0 disables, never a whole-response deadline

    // DoS protection limits
    size_t max_headers = 100;                     ///< Max number of request headers
    size_t max_header_size = 8192;                ///< Max size of a single header line (bytes)
};

/// HTTP server
class server {
public:
    /// Create server with router
    explicit server(router r, server_config config = {})
        : router_(std::move(r)), config_(config) {}
    
    /// Set 404 handler
    template<detail::response_handler Handler>
    void set_not_found_handler(Handler handler) {
        not_found_handler_ = detail::adapt_handler(std::move(handler));
    }
    
    /// Set error handler
    void set_error_handler(std::function<response(const std::exception&)> handler) {
        error_handler_ = std::move(handler);
    }
    
    /// Start listening on address (plain HTTP)
    coro::task<void> listen(const net::socket_address& addr,
                           const net::tcp_options& opts = {}) {
        const auto start_epoch = stop_epoch_.load(std::memory_order_acquire);
        return listen_impl(addr, opts, start_epoch);
    }

    /// Start listening with TLS (HTTPS)
    /// @note The caller must ensure `tls_ctx` outlives all spawned connection
    ///       handlers.  In practice this means `tls_ctx` should be stored as a
    ///       member or otherwise kept alive until after `stop()` returns and
    ///       all in-flight handlers have completed.
    coro::task<void> listen_tls(const net::socket_address& addr,
                                tls::tls_context& tls_ctx,
                                const net::tcp_options& opts = {}) {
        const auto start_epoch = stop_epoch_.load(std::memory_order_acquire);
        return listen_tls_impl(addr, tls_ctx, opts, start_epoch);
    }

    /// Cooperatively cancel accepts and active sessions, including producers.
    /// This does not wait for cleanup or authorize destroying coroutine frames.
    void stop() {
        cancel_active_accepts();
    }

    /// Check if server is running
    bool is_running() const noexcept { return running_; }

    /// Return the number of in-flight connection handlers.  Callers that
    /// destroy the server after stop() must first await every listener task,
    /// then wait until this returns 0. A listener can still be between accept
    /// and connection registration when an earlier zero count is observed.
    size_t active_connections() const noexcept {
        return active_connections_.load(std::memory_order_acquire);
    }

private:
    struct connection_lifetime {
        server& owner;
        std::shared_ptr<coro::cancel_source> source;
        // Retain the listener source after its accept loop exits. A stop that
        // races with spawn still reaches this session through the same token.
        connection_lifetime(server& server, std::shared_ptr<coro::cancel_source> stop)
            : owner(server), source(std::move(stop)) {
            owner.active_connections_.fetch_add(1, std::memory_order_relaxed);
        }
        ~connection_lifetime() {
            owner.active_connections_.fetch_sub(1, std::memory_order_release);
        }
    };

    coro::task<void> listen_impl(const net::socket_address& addr,
                                 const net::tcp_options& opts,
                                 size_t start_epoch) {
        auto* sched = runtime::scheduler::current();
        if (!sched) {
            ELIO_LOG_ERROR("HTTP server must be started from within a scheduler context");
            co_return;
        }

        auto listener_result = net::tcp_listener::bind(addr, opts);
        if (!listener_result) {
            ELIO_LOG_ERROR("Failed to bind HTTP server: {}", strerror(errno));
            co_return;
        }

        ELIO_LOG_INFO("HTTP server listening on {}", addr.to_string());

        auto& listener = *listener_result;
        auto accept_source = begin_accept_loop(start_epoch);
        if (!accept_source) {
            co_return;
        }
        auto accept_token = accept_source->get_token();

        while (running_.load(std::memory_order_acquire)) {
            auto stream_result = co_await listener.accept(accept_token);
            if (accept_loop_should_stop(accept_token)) {
                break;
            }
            if (!stream_result) {
                if (running_.load(std::memory_order_acquire)) {
                    ELIO_LOG_ERROR("Accept error: {}", strerror(errno));
                }
                continue;
            }

            // Spawn connection handler (tracked for graceful shutdown)
            auto lifetime = std::make_shared<connection_lifetime>(*this, accept_source);
            sched->go([this, s = std::move(*stream_result), lifetime]() mutable {
                return handle_connection_guarded(std::move(s), lifetime);
            });
        }
    }

    coro::task<void> listen_tls_impl(const net::socket_address& addr,
                                     tls::tls_context& tls_ctx,
                                     const net::tcp_options& opts,
                                     size_t start_epoch) {
        auto* sched = runtime::scheduler::current();
        if (!sched) {
            ELIO_LOG_ERROR("HTTPS server must be started from within a scheduler context");
            co_return;
        }

        auto listener_result = net::tcp_listener::bind(addr, opts);
        if (!listener_result) {
            ELIO_LOG_ERROR("Failed to bind HTTPS server: {}", strerror(errno));
            co_return;
        }

        ELIO_LOG_INFO("HTTPS server listening on {}", addr.to_string());

        auto& listener = *listener_result;
        auto accept_source = begin_accept_loop(start_epoch);
        if (!accept_source) {
            co_return;
        }
        auto accept_token = accept_source->get_token();

        // Capture tls_ctx by pointer rather than by reference so that the
        // lambda does not silently dangle when listen_tls's coroutine frame
        // is destroyed.  The pointed-to tls_context MUST outlive all spawned
        // handlers — this is documented above.
        auto* tls_ctx_ptr = &tls_ctx;

        while (running_.load(std::memory_order_acquire)) {
            auto stream_result = co_await listener.accept(accept_token);
            if (accept_loop_should_stop(accept_token)) {
                break;
            }
            if (!stream_result) {
                if (running_.load(std::memory_order_acquire)) {
                    ELIO_LOG_ERROR("Accept error: {}", strerror(errno));
                }
                continue;
            }

            // Track in-flight connections for graceful shutdown
            auto lifetime = std::make_shared<connection_lifetime>(*this, accept_source);
            sched->go([this, s = std::move(*stream_result), tls_ctx_ptr, lifetime]() mutable {
                return handle_tls_connection_guarded(std::move(s), *tls_ctx_ptr, lifetime);
            });
        }
    }
    /// Guard wrapper that decrements the active-connection counter on exit.
    coro::task<void> handle_connection_guarded(net::tcp_stream stream,
                                              std::shared_ptr<connection_lifetime> lifetime) {
        co_await handle_connection(std::move(stream), lifetime->source->get_token());
    }

    /// Guard wrapper for TLS connections.
    coro::task<void> handle_tls_connection_guarded(net::tcp_stream tcp, tls::tls_context& tls_ctx,
                                                 std::shared_ptr<connection_lifetime> lifetime) {
        co_await handle_tls_connection(std::move(tcp), tls_ctx, lifetime->source->get_token());
    }

    /// Handle a plain HTTP connection
    coro::task<void> handle_connection(net::tcp_stream stream, coro::cancel_token token) {
        auto peer = stream.peer_address();
        std::string client_addr = peer ? peer->to_string() : "unknown";
        
        if (config_.enable_logging) {
            ELIO_LOG_DEBUG("HTTP connection from {}", client_addr);
        }
        
        co_await handle_requests(stream, client_addr, token);
        
        if (config_.enable_logging) {
            ELIO_LOG_DEBUG("HTTP connection closed: {}", client_addr);
        }
    }
    
    /// Handle a TLS HTTP connection
    coro::task<void> handle_tls_connection(net::tcp_stream tcp, tls::tls_context& tls_ctx,
                                          coro::cancel_token token) {
        auto peer = tcp.peer_address();
        std::string client_addr = peer ? peer->to_string() : "unknown";
        
        if (config_.enable_logging) {
            ELIO_LOG_DEBUG("HTTPS connection from {}", client_addr);
        }
        
        tls::tls_stream stream(std::move(tcp), tls_ctx);
        auto hs_result = co_await detail::perform_tls_handshake_with_timeout(
            stream, config_.keep_alive_timeout, token);
        if (!hs_result.ok) {
            if (hs_result.timed_out) {
                ELIO_LOG_ERROR("TLS handshake timed out for {}", client_addr);
            } else {
                ELIO_LOG_ERROR("TLS handshake failed for {}", client_addr);
            }
            co_return;
        }
        
        co_await handle_requests(stream, client_addr, token);
        
        if (token.is_cancelled()) stream.shutdown_socket();
        else co_await stream.shutdown();
        
        if (config_.enable_logging) {
            ELIO_LOG_DEBUG("HTTPS connection closed: {}", client_addr);
        }
    }
    
    /// Handle HTTP requests on a stream (templated for TCP/TLS)
    template<typename Stream>
    coro::task<void> handle_requests(Stream& stream, const std::string& client_addr,
                                    coro::cancel_token token) {
        auto* sched = runtime::scheduler::current();
        std::vector<char> buffer(std::max<size_t>(1, config_.read_buffer_size));
        request_parser parser;
        parser.set_max_headers(config_.max_headers);
        parser.set_max_header_size(config_.max_header_size);
        size_t request_count = 0;

        while (running_ && !token.is_cancelled() && request_count < config_.max_keep_alive_requests) {
            parser.reset();

            // Slow-loris watchdog: each request is allowed at most
            // keep_alive_timeout to fully arrive. The watchdog sleeps for
            // that duration, then shuts down the socket via the stream's
            // ``shutdown_socket()`` method, which forces any pending recv
            // to return EOF/error so we can exit. Going through the stream
            // (rather than ::shutdown(fd, ...) on a captured fd) lets a
            // tls_stream record that its socket is dead, so its destructor
            // can skip SSL_shutdown — that close_notify write would
            // otherwise risk SIGPIPE on OpenSSL builds without
            // MSG_NOSIGNAL. We must own a join_handle and await it before
            // this scope ends so the watchdog's stream pointer cannot
            // outlive the stream.
            auto timed_out = std::make_shared<std::atomic<bool>>(false);
            auto cancel_src = std::make_shared<coro::cancel_source>();
            auto* stream_ptr = &stream;
            auto timeout = config_.keep_alive_timeout;
            std::optional<coro::join_handle<void>> watchdog;
            if (sched && timeout.count() > 0) {
                watchdog.emplace(sched->go_joinable(
                    [stream_ptr, timed_out, cancel_src, timeout]() -> coro::task<void> {
                        auto r = co_await elio::time::sleep_for(
                            timeout, cancel_src->get_token());
                        if (r == coro::cancel_result::completed) {
                            timed_out->store(true, std::memory_order_release);
                            stream_ptr->shutdown_socket();
                        }
                        co_return;
                    }));
            }

            // Helper that drains the watchdog so the captured fd cannot
            // outlive `stream`. Safe to call multiple times.
            //
            // We explicitly take ownership of the join_handle before
            // awaiting it: ``co_await *watchdog`` is an lvalue await,
            // and ``join_handle`` is intentionally non-copyable, so on
            // some compilers/STDs the lvalue form makes the compiler
            // instantiate the deleted copy ctor while materializing
            // the awaitable. Moving it into a local + ``co_await
            // std::move(wd)`` is the portable, intent-clear form
            // ("one-shot consumption — the handle is gone after
            // this point"). The optional reset happens before the
            // await; subsequent calls to stop_watchdog see no value
            // and skip.
            auto stop_watchdog = [&]() -> coro::task<void> {
                if (watchdog) {
                    cancel_src->cancel();
                    auto wd = std::move(*watchdog);
                    watchdog.reset();
                    co_await std::move(wd);
                }
                co_return;
            };

            // Read and parse request, enforcing max_request_size on every
            // accumulation step so a peer cannot stream gigabytes through
            // the parser before we notice. When a previous keep-alive request
            // left pipelined bytes in the parser, consume those bytes once
            // before waiting for another socket read.
            bool sent_response = false;
            bool early_exit = false;
            bool parse_buffered = parser.buffered_input_size() > 0;
            size_t current_request_size = 0;
            auto send_payload_too_large = [&]() -> coro::task<void> {
                co_await stop_watchdog();
                auto resp = response(status::payload_too_large, "Payload Too Large");
                resp.set_header("Connection", "close");
                co_await send_error_response(stream, std::move(resp), parser.get_method(), parser.version(), token);
                sent_response = true;
                co_return;
            };

            while (!parser.is_complete() && !parser.has_error()) {
                std::string_view input;
                if (parse_buffered) {
                    parse_buffered = false;
                } else {
                    auto result = co_await stream.read(buffer.data(), buffer.size(), token);

                    if (timed_out->load(std::memory_order_acquire)) {
                        early_exit = true;
                        break;
                    }
                    if (result.result <= 0) {
                        early_exit = true;
                        break;
                    }

                    input = std::string_view(buffer.data(),
                                             static_cast<size_t>(result.result));
                }

                auto [pres, consumed] = parser.parse(input);
                current_request_size += consumed;

                // Once the parser has finished headers we know any declared
                // Content-Length. Reject early so we never even allocate the
                // body string for a multi-GB POST.
                if (auto declared = parser.declared_content_length();
                    declared && *declared > config_.max_request_size) {
                    co_await send_payload_too_large();
                    co_return;
                }

                size_t effective_request_size = current_request_size;
                if (!parser.is_complete()) {
                    effective_request_size += parser.buffered_input_size();
                }
                if (effective_request_size > config_.max_request_size ||
                    parser.body().size() > config_.max_request_size) {
                    co_await send_payload_too_large();
                    co_return;
                }

                if (pres == parse_result::error) {
                    co_await stop_watchdog();
                    auto resp = response::bad_request(parser.error_message());
                    resp.set_header("Connection", "close");
                    co_await send_error_response(stream, std::move(resp), parser.get_method(), parser.version(), token);
                    sent_response = true;
                    co_return;
                }
            }

            // Always drain the watchdog before continuing — its captured fd
            // refers to `stream`, which must remain alive until it returns.
            co_await stop_watchdog();

            if (timed_out->load(std::memory_order_acquire) || early_exit ||
                parser.has_error()) {
                (void)sent_response;
                co_return;
            }

            // Create request and context. The interim writer borrows
            // `stream`; context survives handler selection and producer cleanup,
            // so the reference cannot outlive the stream.
            auto req = request::from_parser(parser);
            context ctx(std::move(req), client_addr,
                        [&stream, token](std::string_view data) -> coro::task<bool> {
                            size_t sent = 0;
                            while (sent < data.size()) {
                                auto result = co_await stream.write(
                                    data.data() + sent, data.size() - sent, token);
                                if (result.result <= 0) {
                                    ELIO_LOG_ERROR(
                                        "Failed to send interim response: {}",
                                        result.result == 0
                                            ? "connection closed"
                                            : strerror(-result.result));
                                    co_return false;
                                }
                                sent += static_cast<size_t>(result.result);
                            }
                            co_return true;
                        }, token);

            // Log request
            if (config_.enable_logging) {
                ELIO_LOG_INFO("{} {} {} from {}",
                            method_to_string(ctx.req().get_method()),
                            ctx.req().path(),
                            ctx.req().version(),
                            client_addr);
            }

            // Route request
            reply resp;
            try {
                resp = co_await route_request(ctx);
            } catch (const std::exception& e) {
                ELIO_LOG_ERROR("Handler exception: {}", e.what());
                if (error_handler_) {
                    try {
                        resp = error_handler_(e);
                    } catch (...) {
                        ELIO_LOG_ERROR("Error handler itself threw; falling back to 500");
                        resp = response::internal_error();
                    }
                } else {
                    resp = response::internal_error();
                }
            } catch (...) {
                resp = response::internal_error();
            }

            detail::context_access::seal_final_response(ctx);

            // Supply external reuse permission only. The shared response plan
            // also applies response Connection headers and selected framing.
            bool keep_alive = parser.get_headers().keep_alive(parser.version());
            // If this is the last allowed request, signal close so the
            // client does not pipeline into a connection we are about to
            // abandon (RFC 7230 §6.6).
            if (keep_alive && request_count + 1 >= config_.max_keep_alive_requests) {
                keep_alive = false;
            }
            if (!running_ || token.is_cancelled()) {
                keep_alive = false;
            }

            // Send response
            auto sent = co_await http::send_response(stream, resp,
                parser.get_method(), parser.version(), keep_alive, token, config_.write_timeout);

            if (!sent.success() || !sent.reusable) {
                break;
            }

            ++request_count;
        }
    }
    
    /// Route a request to the appropriate handler
    coro::task<reply> route_request(context& ctx) {
        std::unordered_map<std::string, std::string> params;
        auto* route = router_.find_route(ctx.req().get_method(), ctx.req().path(), params);
        
        if (route) {
            // Set path parameters
            for (const auto& [name, value] : params) {
                ctx.set_param(name, value);
            }
            co_return co_await route->handler(ctx);
        }
        
        // Not found
        if (not_found_handler_) {
            co_return co_await not_found_handler_(ctx);
        }
        
        co_return response::not_found();
    }
    
    template<typename Stream>
    coro::task<void> send_error_response(Stream& stream, response resp,
                                         method request_method, std::string_view version,
                                         coro::cancel_token token) {
        reply selected{std::move(resp)};
        // A malformed request may not yet have a usable version.
        if (version != "HTTP/1.0" && version != "HTTP/1.1") version = "HTTP/1.1";
        (void)co_await http::send_response(stream, selected, request_method, version,
                                          false, std::move(token), config_.write_timeout);
    }

    std::shared_ptr<coro::cancel_source> begin_accept_loop(size_t start_epoch) {
        auto source = std::make_shared<coro::cancel_source>();
        std::lock_guard<std::mutex> lock(accept_cancel_mutex_);
        if (stop_epoch_.load(std::memory_order_acquire) != start_epoch) {
            return {};
        }
        auto it = active_accept_sources_.begin();
        while (it != active_accept_sources_.end()) {
            if (it->expired()) {
                it = active_accept_sources_.erase(it);
            } else {
                ++it;
            }
        }
        active_accept_sources_.push_back(source);
        running_.store(true, std::memory_order_release);
        return source;
    }

    void cancel_active_accepts() {
        std::vector<std::shared_ptr<coro::cancel_source>> sources;
        {
            std::lock_guard<std::mutex> lock(accept_cancel_mutex_);
            stop_epoch_.fetch_add(1, std::memory_order_acq_rel);
            running_.store(false, std::memory_order_release);
            auto it = active_accept_sources_.begin();
            while (it != active_accept_sources_.end()) {
                if (auto source = it->lock()) {
                    sources.push_back(std::move(source));
                    ++it;
                } else {
                    it = active_accept_sources_.erase(it);
                }
            }
        }

        for (auto& source : sources) {
            source->cancel();
        }
    }

    bool accept_loop_should_stop(const coro::cancel_token& accept_token) noexcept {
        if (!running_.load(std::memory_order_acquire) ||
            accept_token.is_cancelled()) {
            running_.store(false, std::memory_order_release);
            return true;
        }
        return false;
    }
    
    router router_;
    server_config config_;
    handler_func not_found_handler_;
    std::function<response(const std::exception&)> error_handler_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> active_connections_{0};  ///< In-flight connection handlers
    std::atomic<size_t> stop_epoch_{0};
    mutable std::mutex accept_cancel_mutex_;
    std::vector<std::weak_ptr<coro::cancel_source>> active_accept_sources_;
};

/// Convenience function to create a simple HTTP server
inline server make_server(router r, server_config config = {}) {
    return server(std::move(r), config);
}

} // namespace elio::http
