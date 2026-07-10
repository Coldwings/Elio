#pragma once

#include <elio/http/http_common.hpp>
#include <elio/http/http_parser.hpp>
#include <elio/http/http_message.hpp>
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
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace elio::http {

/// HTTP request context passed to handlers
class context {
public:
    context(request req, std::string_view client_addr)
        : request_(std::move(req)), client_addr_(client_addr) {}
    
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
    
private:
    request request_;
    std::string client_addr_;
    std::unordered_map<std::string, std::string> params_;
};

/// Handler function type
using handler_func = std::function<coro::task<response>(context&)>;

/// Synchronous handler function type
using sync_handler_func = std::function<response(context&)>;

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
    
    /// Add a route with sync handler
    void add_route(method m, std::string_view pattern, sync_handler_func handler) {
        add_route(m, pattern, [h = std::move(handler)](context& ctx) -> coro::task<response> {
            co_return h(ctx);
        });
    }
    
    /// Convenience methods for common HTTP methods
    void get(std::string_view pattern, handler_func handler) {
        add_route(method::GET, pattern, std::move(handler));
    }
    
    void get(std::string_view pattern, sync_handler_func handler) {
        add_route(method::GET, pattern, std::move(handler));
    }
    
    void post(std::string_view pattern, handler_func handler) {
        add_route(method::POST, pattern, std::move(handler));
    }
    
    void post(std::string_view pattern, sync_handler_func handler) {
        add_route(method::POST, pattern, std::move(handler));
    }
    
    void put(std::string_view pattern, handler_func handler) {
        add_route(method::PUT, pattern, std::move(handler));
    }
    
    void put(std::string_view pattern, sync_handler_func handler) {
        add_route(method::PUT, pattern, std::move(handler));
    }
    
    void del(std::string_view pattern, handler_func handler) {
        add_route(method::DELETE_, pattern, std::move(handler));
    }
    
    void del(std::string_view pattern, sync_handler_func handler) {
        add_route(method::DELETE_, pattern, std::move(handler));
    }
    
    void patch(std::string_view pattern, handler_func handler) {
        add_route(method::PATCH, pattern, std::move(handler));
    }
    
    void options(std::string_view pattern, handler_func handler) {
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
    size_t max_request_size = 10 * 1024 * 1024;  ///< Max request body size (10MB)
    size_t read_buffer_size = 8192;               ///< Read buffer size
    std::chrono::seconds keep_alive_timeout{30};  ///< Keep-alive timeout
    size_t max_keep_alive_requests = 100;         ///< Max requests per connection
    bool enable_logging = true;                   ///< Log requests

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
    void set_not_found_handler(handler_func handler) {
        not_found_handler_ = std::move(handler);
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

    /// Stop the server
    void stop() {
        cancel_active_accepts();
    }

    /// Check if server is running
    bool is_running() const noexcept { return running_; }

    /// Return the number of in-flight connection handlers.  Callers that
    /// destroy the server after stop() should wait until this returns 0
    /// to avoid use-after-free on router_, config_, etc.
    size_t active_connections() const noexcept {
        return active_connections_.load(std::memory_order_acquire);
    }

private:
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
            active_connections_.fetch_add(1, std::memory_order_relaxed);
            sched->go([this, s = std::move(*stream_result)]() mutable {
                return handle_connection_guarded(std::move(s));
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
            active_connections_.fetch_add(1, std::memory_order_relaxed);
            sched->go([this, s = std::move(*stream_result), tls_ctx_ptr]() mutable {
                return handle_tls_connection_guarded(std::move(s), *tls_ctx_ptr);
            });
        }
    }
    /// Guard wrapper that decrements the active-connection counter on exit.
    coro::task<void> handle_connection_guarded(net::tcp_stream stream) {
        co_await handle_connection(std::move(stream));
        active_connections_.fetch_sub(1, std::memory_order_relaxed);
    }

    /// Guard wrapper for TLS connections.
    coro::task<void> handle_tls_connection_guarded(net::tcp_stream tcp, tls::tls_context& tls_ctx) {
        co_await handle_tls_connection(std::move(tcp), tls_ctx);
        active_connections_.fetch_sub(1, std::memory_order_relaxed);
    }

    /// Handle a plain HTTP connection
    coro::task<void> handle_connection(net::tcp_stream stream) {
        auto peer = stream.peer_address();
        std::string client_addr = peer ? peer->to_string() : "unknown";
        
        if (config_.enable_logging) {
            ELIO_LOG_DEBUG("HTTP connection from {}", client_addr);
        }
        
        co_await handle_requests(stream, client_addr);
        
        if (config_.enable_logging) {
            ELIO_LOG_DEBUG("HTTP connection closed: {}", client_addr);
        }
    }
    
    /// Handle a TLS HTTP connection
    coro::task<void> handle_tls_connection(net::tcp_stream tcp, tls::tls_context& tls_ctx) {
        auto peer = tcp.peer_address();
        std::string client_addr = peer ? peer->to_string() : "unknown";
        
        if (config_.enable_logging) {
            ELIO_LOG_DEBUG("HTTPS connection from {}", client_addr);
        }
        
        // Create TLS stream and perform handshake
        tls::tls_stream stream(std::move(tcp), tls_ctx);
        auto hs_result = co_await stream.handshake();
        if (!hs_result) {
            ELIO_LOG_ERROR("TLS handshake failed for {}", client_addr);
            co_return;
        }
        
        co_await handle_requests(stream, client_addr);
        
        co_await stream.shutdown();
        
        if (config_.enable_logging) {
            ELIO_LOG_DEBUG("HTTPS connection closed: {}", client_addr);
        }
    }
    
    /// Handle HTTP requests on a stream (templated for TCP/TLS)
    template<typename Stream>
    coro::task<void> handle_requests(Stream& stream, const std::string& client_addr) {
        auto* sched = runtime::scheduler::current();
        std::vector<char> buffer(config_.read_buffer_size);
        request_parser parser;
        parser.set_max_headers(config_.max_headers);
        parser.set_max_header_size(config_.max_header_size);
        size_t request_count = 0;

        while (running_ && request_count < config_.max_keep_alive_requests) {
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
                co_await this->send_response(stream, resp);
                sent_response = true;
                co_return;
            };

            while (!parser.is_complete() && !parser.has_error()) {
                std::string_view input;
                if (parse_buffered) {
                    parse_buffered = false;
                } else {
                    auto result = co_await stream.read(buffer.data(), buffer.size());

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
                    co_await send_response(stream, resp);
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

            // Create request and context
            auto req = request::from_parser(parser);
            context ctx(std::move(req), client_addr);

            // Log request
            if (config_.enable_logging) {
                ELIO_LOG_INFO("{} {} {} from {}",
                            method_to_string(ctx.req().get_method()),
                            ctx.req().path(),
                            ctx.req().version(),
                            client_addr);
            }

            // Route request
            response resp;
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
            }

            // Check keep-alive: honour both the request AND the response
            // Connection headers, plus the server-side max-requests limit.
            bool keep_alive = parser.get_headers().keep_alive(parser.version());
            if (keep_alive) {
                keep_alive = resp.get_headers().keep_alive(parser.version());
            }
            // If this is the last allowed request, signal close so the
            // client does not pipeline into a connection we are about to
            // abandon (RFC 7230 §6.6).
            if (keep_alive && request_count + 1 >= config_.max_keep_alive_requests) {
                keep_alive = false;
            }
            if (!running_) {
                keep_alive = false;
            }
            if (!keep_alive) {
                resp.set_header("Connection", "close");
            }

            // Send response
            bool send_ok = co_await send_response(stream, resp);

            if (!send_ok || !keep_alive) {
                break;
            }

            ++request_count;
        }
    }
    
    /// Route a request to the appropriate handler
    coro::task<response> route_request(context& ctx) {
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
    
    /// Send HTTP response.  Returns true if the entire response was written
    /// successfully, false on write failure (truncated / broken stream).
    template<typename Stream>
    coro::task<bool> send_response(Stream& stream, const response& resp) {
        auto data = resp.serialize();

        size_t sent = 0;
        while (sent < data.size()) {
            auto result = co_await stream.write(data.data() + sent, data.size() - sent);
            if (result.result <= 0) {
                ELIO_LOG_ERROR("Failed to send HTTP response: {}",
                               result.result == 0 ? "connection closed"
                                                  : strerror(-result.result));
                co_return false;
            }
            sent += result.result;
        }
        co_return true;
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
