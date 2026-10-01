#pragma once

#include <elio/http/http_common.hpp>
#include <elio/http/http_parser.hpp>
#include <elio/http/http_response_reader.hpp>
#include <elio/http/http_response_body_reader.hpp>
#include <elio/http/http_message.hpp>
#include <elio/http/client_base.hpp>
#include <elio/net/stream.hpp>
#include <elio/io/io_context.hpp>
#include <elio/coro/task.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>
#include <elio/log/macros.hpp>

#include <sys/socket.h>

#include <atomic>
#include <string>
#include <string_view>
#include <memory>
#include <array>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <concepts>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace elio::http {

#ifdef ELIO_RUNTIME_TEST_HOOKS
namespace detail {
// Expire only the Expect clock after final headers, allowing a regression to
// hold the final body behind a barrier without relying on timer scheduling.
inline std::atomic<bool> expire_expect_after_headers_for_test{false};
inline std::atomic<bool> final_headers_seen_for_test{false};
// Inject a terminal write result without depending on TCP close timing.
using request_write_hook = io::io_result (*)(std::string_view);
inline std::atomic<request_write_hook> request_write_result_for_test{nullptr};
inline std::atomic<client_stage> response_read_stage_for_test{client_stage::headers};
using response_headers_hook = void (*)(uint16_t);
inline std::atomic<response_headers_hook> response_headers_for_test{nullptr};
using response_deadline_hook = void (*)(std::chrono::steady_clock::time_point);
inline std::atomic<response_deadline_hook> response_deadline_for_test{nullptr};
using response_watchdog_wait_hook = coro::task<coro::cancel_result> (*)(
    std::chrono::nanoseconds, coro::cancel_token, client_stage);
inline std::atomic<response_watchdog_wait_hook> response_watchdog_wait_for_test{nullptr};
} // namespace detail
#endif

/// HTTP client configuration
struct client_config : base_client_config {
    size_t max_redirects = 5;                     ///< Max redirects to follow
    bool follow_redirects = true;                 ///< Auto-follow redirects
    size_t max_connections_per_host = 6;          ///< Max connections per host
    std::chrono::seconds pool_idle_timeout{60};   ///< Idle connection timeout
    /// Buffered APIs cap the accumulated final body and, separately, the
    /// cumulative informational wire bytes. with_response uses its own options
    /// for these limits and never allocates a body-sized receive buffer.
    size_t max_response_size = 16 * 1024 * 1024;  ///< Max response body size (16 MiB)
    /// Deadline for waiting on an interim 100 Continue when a request uses
    /// request::set_expect_continue(). On expiry the body is sent anyway
    /// (RFC 9110 §10.1.1 fallback). A value <= 0 skips the wait: the body
    /// is sent immediately after the headers. The wait is additionally
    /// bounded by the absolute response deadline (read_timeout), whichever
    /// expires first; response-deadline expiry fails the request with
    /// ETIMEDOUT instead of triggering the fallback.
    std::chrono::milliseconds expect_continue_timeout{1000};

    client_config() {
        user_agent = "elio-http/1.0";
    }
};

/// Connection wrapper using unified net::stream
using connection = net::stream;

/// Connection pool for HTTP keep-alive
class connection_pool {
public:
    static constexpr size_t shard_count = 16;

    /// Per-acquisition snapshot; an explicit null domain selects the shared
    /// resolver default instead of retaining a constructor-time custom domain.
    struct dns_options {
        std::chrono::nanoseconds timeout{0};
        std::shared_ptr<net::resolve_domain> domain{};
    };

    explicit connection_pool(client_config config = {})
        : config_(config) {}
    
    /// Get or create a connection to host
    coro::task<client_result<connection>> acquire_result(const std::string& host,
                                                   uint16_t port,
                                                   bool secure,
                                                   tls::tls_context* tls_ctx = nullptr,
                                                   std::chrono::nanoseconds connect_timeout =
                                                       std::chrono::nanoseconds::zero(),
                                                   coro::cancel_token token = {},
                                                   std::optional<dns_options> dns = std::nullopt) {
        std::string key = make_key(host, port, secure);
        auto& shard = shard_for(key);

        // Try to get an existing connection.  Extract it under the lock
        // into a local variable, then release the lock BEFORE any
        // suspension point (co_return) so we never hold a std::mutex
        // across a coroutine suspension.
        std::optional<connection> conn;
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            auto it = shard.pools.find(key);
            if (it != shard.pools.end() && !it->second.empty()) {
                auto candidate = std::move(it->second.front());
                it->second.pop_front();

                // Check if connection is still valid (not too old)
                auto age = std::chrono::steady_clock::now() - candidate.last_use();
                if (age < config_.pool_idle_timeout) {
                    candidate.touch();
                    conn = std::move(candidate);
                }
                // Connection too old, let it close
            }
        }
        if (conn.has_value()) {
            co_return std::move(*conn);
        }
        if (token.is_cancelled()) {
            co_return detail::make_client_error(ECANCELED, client_stage::acquire);
        }

        // Create new connection using client_connect utility
        auto result = co_await client_connect_result(
            host,
            port,
            secure,
            tls_ctx,
            config_.resolve_options,
            config_.rotate_resolved_addresses,
            connect_timeout,
            std::move(token),
            dns ? dns->timeout : config_.dns_timeout,
            dns ? dns->domain : config_.dns_domain);
        co_return std::move(result);
    }

    coro::task<std::optional<connection>> acquire(const std::string& host,
            uint16_t port, bool secure, tls::tls_context* tls_ctx = nullptr,
            std::chrono::nanoseconds connect_timeout = std::chrono::nanoseconds::zero(),
            coro::cancel_token token = {},
            std::optional<dns_options> dns = std::nullopt) {
        auto result = co_await acquire_result(host, port, secure, tls_ctx,
                                             connect_timeout, std::move(token), std::move(dns));
        if (const auto* error = std::get_if<client_error>(&result)) {
            errno = error->code.value();
            co_return std::nullopt;
        }
        co_return std::move(std::get<connection>(result));
    }
    
    /// Return a connection to the pool
    void release(const std::string& host, uint16_t port, bool secure, connection conn) {
        std::string key = make_key(host, port, secure);
        auto& shard = shard_for(key);
        
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto& pool = shard.pools[key];
        
        if (pool.size() < config_.max_connections_per_host) {
            conn.touch();
            pool.push_back(std::move(conn));
        }
        // Otherwise let connection close
    }
    
    /// Clear all pooled connections
    void clear() {
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.pools.clear();
        }
    }
    
private:
    static std::string make_key(const std::string& host, uint16_t port, bool secure) {
        return (secure ? "https://" : "http://") + host + ":" + std::to_string(port);
    }

    struct pool_shard {
        std::mutex mutex;
        std::unordered_map<std::string, std::deque<connection>> pools;
    };

    pool_shard& shard_for(const std::string& key) noexcept {
        return shards_[std::hash<std::string>{}(key) % shard_count];
    }
    
    client_config config_;
    std::array<pool_shard, shard_count> shards_;
};

/// HTTP client
class client {
public:
    /// Create client with default configuration
    client() : client(client_config{}) {}

    /// Create client with configuration
    explicit client(client_config config)
        : config_(config)
        , pool_(config)
        , tls_ctx_(tls::tls_mode::client) {
        // Setup TLS context using shared utility
        init_client_tls_context(tls_ctx_, config_.verify_certificate);
    }
    
    /// Perform HTTP GET request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> get(std::string_view url_str) {
        return request_url(method::GET, url_str, "", "", coro::cancel_token{});
    }
    
    /// Perform HTTP GET request with cancellation support
    coro::task<std::optional<response>> get(std::string_view url_str, coro::cancel_token token) {
        return request_url(method::GET, url_str, "", "", std::move(token));
    }
    
    /// Perform HTTP POST request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> post(std::string_view url_str, 
                                                   std::string_view body,
                                                   std::string_view content_type = mime::application_form_urlencoded) {
        return request_url(method::POST, url_str, body, content_type, coro::cancel_token{});
    }
    
    /// Perform HTTP POST request with cancellation support
    coro::task<std::optional<response>> post(std::string_view url_str, 
                                                   std::string_view body,
                                                   coro::cancel_token token,
                                                   std::string_view content_type = mime::application_form_urlencoded) {
        return request_url(method::POST, url_str, body, content_type, std::move(token));
    }
    
    /// Perform HTTP PUT request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> put(std::string_view url_str,
                                                  std::string_view body,
                                                  std::string_view content_type = mime::application_json) {
        return request_url(method::PUT, url_str, body, content_type, coro::cancel_token{});
    }
    
    /// Perform HTTP PUT request with cancellation support
    coro::task<std::optional<response>> put(std::string_view url_str,
                                                  std::string_view body,
                                                  coro::cancel_token token,
                                                  std::string_view content_type = mime::application_json) {
        return request_url(method::PUT, url_str, body, content_type, std::move(token));
    }
    
    /// Perform HTTP DELETE request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> del(std::string_view url_str) {
        return request_url(method::DELETE_, url_str, "", "", coro::cancel_token{});
    }
    
    /// Perform HTTP DELETE request with cancellation support
    coro::task<std::optional<response>> del(std::string_view url_str, coro::cancel_token token) {
        return request_url(method::DELETE_, url_str, "", "", std::move(token));
    }
    
    /// Perform HTTP PATCH request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> patch(std::string_view url_str,
                                                    std::string_view body,
                                                    std::string_view content_type = mime::application_json) {
        return request_url(method::PATCH, url_str, body, content_type, coro::cancel_token{});
    }
    
    /// Perform HTTP PATCH request with cancellation support
    coro::task<std::optional<response>> patch(std::string_view url_str,
                                                    std::string_view body,
                                                    coro::cancel_token token,
                                                    std::string_view content_type = mime::application_json) {
        return request_url(method::PATCH, url_str, body, content_type, std::move(token));
    }
    
    /// Perform HTTP HEAD request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> head(std::string_view url_str) {
        return request_url(method::HEAD, url_str, "", "", coro::cancel_token{});
    }
    
    /// Perform HTTP HEAD request with cancellation support
    coro::task<std::optional<response>> head(std::string_view url_str, coro::cancel_token token) {
        return request_url(method::HEAD, url_str, "", "", std::move(token));
    }
    
    /// Send a custom request
    /// @return Response on success, std::nullopt on error (check errno)
    coro::task<std::optional<response>> send(request& req, const url& target) {
        co_return co_await send(req, target, coro::cancel_token{});
    }
    
    /// Send a custom request with cancellation support
    coro::task<std::optional<response>> send(request& req, const url& target, coro::cancel_token token) {
        co_return optional_response(co_await send_result(req, target, std::move(token)));
    }

    /// Owned operational errors; allocation/setup/programming exceptions may
    /// propagate. Keep this client, req, and target alive through awaited return.
    coro::task<client_result<response>> send_result(
            request& req, const url& target, coro::cancel_token token = {}) {
        if (!detail::is_supported_http_url_scheme(target.scheme)) {
            co_return detail::make_client_error(EINVAL, client_stage::target);
        }
        co_return co_await send_request(req, target, 0, std::move(token));
    }

    coro::task<client_result<response>> get_result(
            std::string_view url_str, coro::cancel_token token = {}) {
        return request_result(method::GET, url_str, "", "", std::move(token));
    }

    /// Owns request, target, and handler before lazy execution. Keep this client
    /// alive and unmoved through awaited return. The handler receives final
    /// headers (response.body() is empty), a scoped body reader, and the token.
    /// The token argument is read-only; by-value handlers receive a copy.
    /// It must await all reads before returning. Early return closes rather
    /// than drains; exceptions close the exchange and propagate unchanged.
    /// read_timeout retains its original per-hop I/O deadline across body
    /// pulls, but does not preempt handler code or include DNS/connect time.
    template<typename Handler>
        requires std::invocable<Handler&, const response&, response_body_reader&,
                                const coro::cancel_token&> &&
                 std::same_as<std::invoke_result_t<Handler&, const response&,
                     response_body_reader&, const coro::cancel_token&>, coro::task<void>>
    coro::task<client_result<std::monostate>> with_response(
            request req, url target, coro::cancel_token token, Handler handler,
            streaming_response_options options = {}) {
        return with_response_impl(std::move(req), std::move(target), std::move(token),
                                  std::move(handler), options);
    }
    
    /// Get TLS context for configuration
    tls::tls_context& tls_context() noexcept { return tls_ctx_; }
    
    /// Get configuration
    client_config& config() noexcept { return config_; }
    const client_config& config() const noexcept { return config_; }
    
    /// Method-general value API. Borrowed string inputs and this client must
    /// remain valid through awaited return; successful HTTP statuses are values.
    coro::task<client_result<response>> request_result(method m,
                                                          std::string_view url_str,
                                                          std::string_view body = {},
                                                          std::string_view content_type = {},
                                                          coro::cancel_token token = {}) {
        return request_url_result(m, url_str, body, content_type, std::move(token), true);
    }

private:
    coro::task<client_result<response>> request_url_result(
            method m, std::string_view url_str, std::string_view body,
            std::string_view content_type, coro::cancel_token token,
            bool honor_empty_representation) {
        // Check if already cancelled
        if (token.is_cancelled()) {
            co_return detail::make_client_error(ECANCELED, client_stage::target);
        }
        
        auto parsed = url::parse(url_str);
        if (!parsed) {
            co_return detail::make_client_error(EINVAL, client_stage::target);
        }
        if (!detail::is_supported_http_url_scheme(parsed->scheme)) {
            co_return detail::make_client_error(EINVAL, client_stage::target);
        }

        if (!config_.user_agent.empty() &&
            !detail::is_valid_header_value(config_.user_agent)) {
            co_return detail::make_client_error(EINVAL, client_stage::request);
        }
        if (!content_type.empty() &&
            !detail::is_valid_header_value(content_type)) {
            co_return detail::make_client_error(EINVAL, client_stage::request);
        }
        
        request req(m, parsed->path_with_query());
        req.set_host(parsed->host_authority());
        
        if (!body.empty() || (honor_empty_representation && !content_type.empty())) {
            req.set_body(body);
            if (!content_type.empty()) {
                req.set_content_type(content_type);
            }
        }
        
        if (!config_.user_agent.empty()) {
            req.set_header("User-Agent", config_.user_agent);
        }
        
        co_return co_await send_request(req, *parsed, 0, std::move(token));
    }

    static std::optional<response> optional_response(client_result<response> result) {
        if (const auto* error = std::get_if<client_error>(&result)) {
            errno = error->code.value();
            return std::nullopt;
        }
        return std::move(std::get<response>(result));
    }

    coro::task<std::optional<response>> request_url(method m, std::string_view url_str,
            std::string_view body, std::string_view content_type, coro::cancel_token token) {
        // Preserve legacy empty-body serialization; the value API honors an
        // explicit content type even for a zero-length representation.
        co_return optional_response(co_await request_url_result(
            m, url_str, body, content_type, std::move(token), false));
    }
    
    static bool is_informational_status(uint16_t code) noexcept {
        return code >= 100 && code < 200;
    }

    static client_stage response_read_stage(const response_decoder& decoder) noexcept {
        return decoder.headers_complete() && !is_informational_status(decoder.status_code())
            ? client_stage::body : client_stage::headers;
    }

    /// Write the whole buffer to the connection, optionally bounded by
    /// `io_deadline`. read_timeout doubles as the send deadline: a stalled
    /// write to a malicious server is the same liveness problem as a
    /// stalled read, so the same bound applies. The watchdog shutdown(2)s
    /// the fd to abort an in-flight write on timeout. Returns an owned error
    /// on failure and an empty optional on success.
    static coro::task<std::optional<client_error>>
    write_request_data(connection& conn, std::string_view data,
                       const url& target,
                       std::chrono::nanoseconds io_deadline,
                       bool deadline_enforced,
                       runtime::scheduler* sched,
                       const coro::cancel_token& token) {
        io::io_result write_result{};
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = detail::request_write_result_for_test.load(std::memory_order_acquire)) {
            write_result = hook(data);
        } else
#endif
        if (deadline_enforced) {
            auto timed_out = std::make_shared<std::atomic<bool>>(false);
            write_result = co_await detail::await_fd_operation_with_watchdog(
                [&] { return conn.write_all(data, token); },
                sched, conn.fd(), io_deadline, timed_out);
            if (timed_out->load(std::memory_order_acquire)) {
                conn.mark_externally_shut_down();
                ELIO_LOG_ERROR("Write to {}:{} timed out after {}s",
                               target.host, target.effective_port(),
                               std::chrono::duration_cast<std::chrono::seconds>(io_deadline).count());
                co_return detail::make_client_error(ETIMEDOUT, client_stage::request);
            }
        } else {
            write_result = co_await conn.write_all(data, token);
        }
        if (write_result.result <= 0) {
            if (write_result.result == -ECANCELED && token.is_cancelled()) {
                detail::abort_stream_io(conn);
                co_return detail::make_client_error(ECANCELED, client_stage::request);
            }
            ELIO_LOG_ERROR("Failed to send request: {}",
                           write_result.result == 0 ? "connection closed"
                                                    : strerror(-write_result.result));
            co_return detail::make_client_error(
                write_result.result == 0 ? ECONNRESET : -write_result.result,
                client_stage::request);
        }
        co_return std::nullopt;
    }

    static bool is_auto_follow_redirect(status s) noexcept {
        switch (s) {
        case status::moved_permanently:
        case status::found:
        case status::see_other:
        case status::temporary_redirect:
        case status::permanent_redirect:
            return true;
        default:
            return false;
        }
    }

    class exchange_state final {
    public:
        exchange_state(connection conn, const client_config& config, const url& target,
                       method request_method, std::string_view request_body, bool defer_body,
                       size_t informational_limit)
            : conn_(std::move(conn)), config_(config), target_(target),
              reader_(config.read_buffer_size), request_method_(request_method),
              request_body_(request_body), informational_limit_(informational_limit),
              body_pending_(defer_body),
              scheduler_(runtime::scheduler::current()),
              deadline_enforced_(scheduler_ && config.read_timeout.count() > 0),
              io_deadline_(config.read_timeout),
              response_deadline_(std::chrono::steady_clock::now() + io_deadline_),
              expect_bounded_(scheduler_ && config.expect_continue_timeout.count() > 0) {
            reader_.set_max_headers(config.max_headers);
            reader_.set_max_header_size(config.max_header_size);
            reader_.set_request_method(request_method_);
        }

        exchange_state(const exchange_state&) = delete;
        exchange_state& operator=(const exchange_state&) = delete;
        exchange_state(exchange_state&&) = delete;
        exchange_state& operator=(exchange_state&&) = delete;

        coro::task<std::optional<client_error>> send_initial(
                std::string_view data, coro::cancel_token token) {
            if (auto error = co_await write_request_data(conn_, data, target_,
                    io_deadline_, deadline_enforced_, scheduler_, token)) {
                co_return error;
            }
            expect_deadline_ = std::chrono::steady_clock::now() +
                config_.expect_continue_timeout;
            if (body_pending_ && !expect_bounded_) {
                co_return co_await send_pending_body(token);
            }
            co_return std::nullopt;
        }

        coro::task<client_result<response_read_result>> next(coro::cancel_token token) {
            auto receive = [this, token](void* data, size_t size) {
                return receive_data(data, size, token);
            };
            while (true) {
                if (token.is_cancelled()) {
                    co_return detail::make_client_error(ECANCELED,
                        response_read_stage(reader_.decoder()));
                }
                auto part = co_await reader_.read_with(receive, token);
                if (!part.success()) {
                    if (expect_expired_ && body_pending_ && !token.is_cancelled()) {
                        if (auto error = co_await send_pending_body(token)) co_return *error;
                        continue;
                    }
                    const auto stage = reader_.decoder().has_error() ? client_stage::framing
                        : response_read_stage(reader_.decoder());
                    co_return detail::make_client_error(part.error, stage);
                }
                const auto code = reader_.decoder().status_code();
                if (part.event == response_event::headers_complete) {
#ifdef ELIO_RUNTIME_TEST_HOOKS
                    if (auto hook = detail::response_headers_for_test.load(std::memory_order_acquire)) {
                        hook(code);
                    }
                    if (!is_informational_status(code) &&
                        detail::expire_expect_after_headers_for_test.load(std::memory_order_acquire)) {
                        expect_deadline_ = std::chrono::steady_clock::now();
                        detail::client_response_read_staged_for_test.store(false, std::memory_order_release);
                        detail::final_headers_seen_for_test.store(true, std::memory_order_release);
                    }
#endif
                    if (code == static_cast<uint16_t>(status::switching_protocols) ||
                        (request_method_ == method::CONNECT && code >= 200 && code < 300)) {
                        co_return detail::make_client_error(EBADMSG, client_stage::framing);
                    }
                    if (!is_informational_status(code)) {
                        body_pending_ = false;
                        co_return part;
                    }
                    if (code == static_cast<uint16_t>(status::continue_)) {
                        if (auto error = co_await send_pending_body(token)) co_return *error;
                    }
                } else if (part.event == response_event::protocol_handoff) {
                    co_return detail::make_client_error(EBADMSG, client_stage::framing);
                } else if (part.event == response_event::message_complete) {
                    if (!is_informational_status(code)) co_return part;
                    if (reader_.message_bytes() > informational_limit_ -
                        skipped_informational_bytes_) {
                        co_return detail::make_client_error(EMSGSIZE, client_stage::framing);
                    }
                    skipped_informational_bytes_ += reader_.message_bytes();
                    if (!reader_.next_response()) {
                        co_return detail::make_client_error(EBADMSG, client_stage::framing);
                    }
                    reader_.set_request_method(request_method_);
                } else if (part.event == response_event::body) {
                    co_return part;
                }
            }
        }

        const response_reader& reader() const noexcept { return reader_; }

        bool reusable() const {
            return reader_.decoder().is_complete() &&
                !reader_.decoder().is_close_delimited() && !reader_.reached_eof() &&
                reader_.bytes_remaining() == 0 &&
                reader_.decoder().get_headers().keep_alive(reader_.decoder().version());
        }

        connection take_connection() noexcept { return std::move(conn_); }
        void abort() noexcept { detail::abort_stream_io(conn_); }

    private:
        coro::task<std::optional<client_error>> send_pending_body(
                const coro::cancel_token& token) {
            if (!body_pending_) co_return std::nullopt;
            body_pending_ = false;
            const auto remaining = response_deadline_ - std::chrono::steady_clock::now();
            if (deadline_enforced_ && remaining <= std::chrono::steady_clock::duration::zero()) {
                co_return detail::make_client_error(ETIMEDOUT, client_stage::request);
            }
            co_return co_await write_request_data(conn_, request_body_, target_,
                deadline_enforced_ ? remaining : io_deadline_, deadline_enforced_, scheduler_, token);
        }

        coro::task<io::io_result> receive_data(void* data, size_t size,
                                             coro::cancel_token token) {
            expect_expired_ = false;
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (auto hook = detail::response_deadline_for_test.load(std::memory_order_acquire)) {
                hook(response_deadline_);
            }
#endif
            const auto now = std::chrono::steady_clock::now();
            if (deadline_enforced_ && now >= response_deadline_) {
                co_return io::io_result{-ETIMEDOUT, 0};
            }
            const bool waiting_expect = body_pending_ && expect_bounded_;
            if (waiting_expect && now >= expect_deadline_) {
                expect_expired_ = true;
                co_return io::io_result{-ETIMEDOUT, 0};
            }
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (detail::observe_client_response_read_entry_for_test.load(std::memory_order_acquire)) {
                detail::client_response_read_staged_for_test.store(false, std::memory_order_release);
                detail::response_read_stage_for_test.store(
                    response_read_stage(reader_.decoder()), std::memory_order_release);
            }
            detail::arm_client_response_read_observer_for_test();
#endif
            if (!deadline_enforced_ && !waiting_expect) {
                co_return co_await conn_.read(data, size, token);
            }
            const bool expect_first = waiting_expect &&
                (!deadline_enforced_ || expect_deadline_ < response_deadline_);
            const auto deadline = expect_first ? expect_deadline_ : response_deadline_;
            auto read_cancel = std::make_shared<coro::cancel_source>();
            auto forward = token.on_cancel([read_cancel] { read_cancel->cancel(); });
            auto expired = std::make_shared<std::atomic<bool>>(false);
            coro::cancel_source watchdog_cancel;
            auto watchdog = scheduler_->go_joinable(
                [deadline, expired, read_cancel,
                 stage = response_read_stage(reader_.decoder()),
                 stop = watchdog_cancel.get_token()]() -> coro::task<void> {
                    coro::cancel_result result;
                    try {
                        const auto remaining = deadline - std::chrono::steady_clock::now();
#ifdef ELIO_RUNTIME_TEST_HOOKS
                        if (auto hook = detail::response_watchdog_wait_for_test.load(
                                std::memory_order_acquire)) {
                            result = co_await hook(remaining, stop, stage);
                        } else
#endif
                        {
                            (void)stage;
                            result = co_await elio::time::sleep_for(remaining, stop);
                        }
                    } catch (...) {
                        // A failed timer must release its sibling read. Keep
                        // that exception even if cancellation callbacks fail.
                        auto failure = std::current_exception();
                        try { read_cancel->cancel(); } catch (...) {}
                        std::rethrow_exception(failure);
                    }
                    if (result == coro::cancel_result::completed) {
                        expired->store(true, std::memory_order_release);
                        read_cancel->cancel();
                    }
                });
            io::io_result result{};
            std::exception_ptr failure;
            try {
                result = co_await conn_.read(data, size, read_cancel->get_token());
            } catch (...) {
                failure = std::current_exception();
            }
            try {
                watchdog_cancel.cancel();
            } catch (...) {
                if (!failure) failure = std::current_exception();
            }
            try {
                co_await watchdog;
            } catch (...) {
                if (!failure) failure = std::current_exception();
            }
            try {
                co_await watchdog.wait_destroyed_async();
            } catch (...) {
                if (!failure) failure = std::current_exception();
            }
            if (failure) std::rethrow_exception(failure);
            if (expired->load(std::memory_order_acquire)) {
                if (expect_first && result.result > 0) co_return result;
                expect_expired_ = expect_first;
                co_return io::io_result{-ETIMEDOUT, 0};
            }
            co_return result;
        }

        connection conn_;
        const client_config& config_;
        const url& target_;
        response_reader reader_;
        method request_method_;
        std::string_view request_body_;
        size_t informational_limit_;
        size_t skipped_informational_bytes_ = 0;
        bool body_pending_;
        runtime::scheduler* scheduler_;
        bool deadline_enforced_;
        std::chrono::nanoseconds io_deadline_;
        std::chrono::steady_clock::time_point response_deadline_;
        bool expect_bounded_;
        std::chrono::steady_clock::time_point expect_deadline_{};
        bool expect_expired_ = false;
    };

    coro::task<client_result<std::unique_ptr<exchange_state>>> prepare_exchange(
            request& req, const url& target, coro::cancel_token token,
            size_t informational_limit) {
        if (token.is_cancelled()) {
            co_return detail::make_client_error(ECANCELED, client_stage::acquire);
        }
        if (!detail::is_valid_url_input(target.host_authority()) ||
            !detail::is_valid_request_target(req.path_with_query())) {
            ELIO_LOG_ERROR("Invalid outbound HTTP request target");
            co_return detail::make_client_error(EINVAL, client_stage::target);
        }
        // Keep the owning snapshot and task construction outside the await
        // expression; some coroutine toolchains mishandle aggregate temporaries.
        std::optional<connection_pool::dns_options> dns{std::in_place};
        dns->timeout = config_.dns_timeout;
        dns->domain = config_.dns_domain;
        auto acquisition = pool_.acquire_result(target.host, target.effective_port(),
            target.is_secure(), &tls_ctx_, config_.connect_timeout, token,
            std::move(dns));
        auto conn_result = co_await std::move(acquisition);
        if (const auto* error = std::get_if<client_error>(&conn_result)) co_return *error;
        auto conn = std::move(std::get<connection>(conn_result));
        if (req.header("Host").empty()) req.set_host(target.host_authority());
        if (!req.get_headers().contains("Connection")) {
            req.set_header("Connection", "keep-alive");
        }
        const bool defer_body = req.expect_continue() && !req.body().empty();
        std::string request_data;
        try {
            request_data = defer_body ? req.serialize_headers() : req.serialize();
        } catch (const std::invalid_argument& ex) {
            ELIO_LOG_ERROR("Invalid outbound HTTP request: {}", ex.what());
            co_return detail::make_client_error(EINVAL, client_stage::request);
        }
        ELIO_LOG_DEBUG("Sending request to {}:{}\n{}", target.host,
                       target.effective_port(), request_data);
        if (token.is_cancelled()) {
            co_return detail::make_client_error(ECANCELED, client_stage::request);
        }
        auto exchange = std::make_unique<exchange_state>(std::move(conn), config_, target,
            req.get_method(), req.body(), defer_body, informational_limit);
        if (auto error = co_await exchange->send_initial(request_data, token)) co_return *error;
        co_return std::move(exchange);
    }

    struct redirect_request {
        request req;
        url target;
    };

    std::optional<redirect_request> make_redirect(
            const request& req, const url& target, const response& resp,
            size_t redirect_count) const {
        if (!config_.follow_redirects || !is_auto_follow_redirect(resp.get_status()) ||
            redirect_count >= config_.max_redirects) return std::nullopt;
        auto location = resp.header("Location");
        if (location.empty()) return std::nullopt;
        ELIO_LOG_DEBUG("Following redirect to: {}", location);
        if (!detail::is_valid_url_input(location)) {
            ELIO_LOG_WARNING("Rejecting invalid redirect Location: {}", location);
            return std::nullopt;
        }
        auto redirect_url = url::resolve_reference(target, location);
        if (!redirect_url) return std::nullopt;
        if (!detail::is_supported_http_url_scheme(redirect_url->scheme)) {
            ELIO_LOG_WARNING("Rejecting unsupported redirect scheme: {}", redirect_url->scheme);
            return std::nullopt;
        }
        if (target.is_secure() && !redirect_url->is_secure()) {
            ELIO_LOG_WARNING("Rejecting insecure redirect from HTTPS to HTTP: {}", location);
            return std::nullopt;
        }
        method redirect_method = req.get_method();
        if ((resp.get_status() == status::see_other && req.get_method() != method::HEAD) ||
            ((resp.get_status() == status::moved_permanently || resp.get_status() == status::found) &&
             req.get_method() == method::POST)) {
            redirect_method = method::GET;
        }
        request redirect_req(redirect_method, redirect_url->path_with_query());
        redirect_req.set_host(redirect_url->host_authority());
        if (!config_.user_agent.empty()) redirect_req.set_header("User-Agent", config_.user_agent);
        const bool method_preserved = redirect_method == req.get_method() &&
            resp.get_status() != status::see_other;
        if (resp.get_status() == status::temporary_redirect ||
            resp.get_status() == status::permanent_redirect || method_preserved) {
            if (!req.body().empty()) {
                redirect_req.set_body(req.body());
            } else if (req.get_headers().content_length() == size_t{0}) {
                redirect_req.set_header("Content-Length", req.get_headers().get("Content-Length"));
            }
            if (!req.content_type().empty()) redirect_req.set_content_type(req.content_type());
            if (!req.body().empty() && req.expect_continue()) redirect_req.set_expect_continue();
        }
        return redirect_request{std::move(redirect_req), std::move(*redirect_url)};
    }

    template<typename Handler>
    coro::task<client_result<std::monostate>> with_response_impl(
            request req, url target, coro::cancel_token token, Handler handler,
            streaming_response_options options) {
        if (!detail::is_supported_http_url_scheme(target.scheme)) {
            co_return detail::make_client_error(EINVAL, client_stage::target);
        }
        for (size_t redirects = 0;; ++redirects) {
            auto prepared = co_await prepare_exchange(req, target, token,
                                                      options.max_informational_bytes);
            if (const auto* error = std::get_if<client_error>(&prepared)) co_return *error;
            auto exchange = std::move(std::get<std::unique_ptr<exchange_state>>(prepared));
            while (true) {
                auto next = co_await exchange->next(token);
                if (const auto* error = std::get_if<client_error>(&next)) {
                    exchange->abort();
                    co_return *error;
                }
                if (std::get<response_read_result>(next).event == response_event::headers_complete) break;
            }
            auto head = response::from_decoder(exchange->reader().decoder(), {});
            if (auto redirect = make_redirect(req, target, head, redirects)) {
                // The state borrows the current request/target; retire it before
                // replacing either, and never pool an unconsumed redirect body.
                exchange->abort();
                exchange.reset();
                req = std::move(redirect->req);
                target = std::move(redirect->target);
                continue;
            }
            response_body_reader body(
                [state = exchange.get()](coro::cancel_token read_token) {
                    return state->next(std::move(read_token));
                }, token, options.max_body_size);
            std::exception_ptr failure;
            try {
                co_await std::invoke(handler, std::as_const(head), body, std::as_const(token));
            } catch (...) {
                failure = std::current_exception();
            }
            if (failure) {
                exchange->abort();
                std::rethrow_exception(failure);
            }
            if (body.error()) {
                exchange->abort();
                co_return *body.error();
            }
            if (token.is_cancelled()) {
                exchange->abort();
                co_return detail::make_client_error(ECANCELED, client_stage::body);
            }
            if (body.complete() && exchange->reusable()) {
                pool_.release(target.host, target.effective_port(), target.is_secure(),
                              exchange->take_connection());
            } else {
                exchange->abort();
            }
            co_return std::monostate{};
        }
    }

    /// Send request with redirect handling
    coro::task<client_result<response>> send_request(request& req, const url& target,
                                                           size_t redirect_count,
                                                           coro::cancel_token token) {
        auto prepared = co_await prepare_exchange(req, target, token, config_.max_response_size);
        if (const auto* error = std::get_if<client_error>(&prepared)) co_return *error;
        auto exchange = std::move(std::get<std::unique_ptr<exchange_state>>(prepared));
        std::string response_body;

        while (true) {
            auto next = co_await exchange->next(token);
            if (const auto* error = std::get_if<client_error>(&next)) co_return *error;
            auto part = std::get<response_read_result>(next);
            if (part.event == response_event::body) {
                if (part.body.size() > config_.max_response_size -
                    std::min(response_body.size(), config_.max_response_size)) {
                    co_return detail::make_client_error(EMSGSIZE, client_stage::body);
                }
                response_body.append(part.body);
            } else if (part.event == response_event::message_complete) {
                break;
            }
        }
        auto resp = response::from_decoder(exchange->reader().decoder(), std::move(response_body));

        // Return connection to pool only when (a) keep-alive is allowed by
        // the response, (b) the parser is in the complete state, and (c) no
        // bytes are left over in the parser's input buffer. A non-empty
        // remaining buffer means the server pipelined extra bytes after the
        // response — pooling the conn would let those bytes be misread as the
        // head of the next response (response-splitting). On any failure of
        // these conditions the connection is simply dropped on scope exit.
        if (exchange->reusable()) {
            pool_.release(target.host, target.effective_port(), target.is_secure(),
                          exchange->take_connection());
        }
        
        if (auto redirect = make_redirect(req, target, resp, redirect_count)) {
            co_return co_await send_request(redirect->req, redirect->target,
                                          redirect_count + 1, token);
        }
        
        co_return resp;
    }
    
    client_config config_;
    connection_pool pool_;
    tls::tls_context tls_ctx_;
};

/// Simple convenience functions for one-off requests

/// Perform HTTP GET request
/// @return Response on success, std::nullopt on error (check errno)
inline coro::task<std::optional<response>> get(std::string_view url) {
    client c;
    co_return co_await c.get(url);
}

/// Perform HTTP GET request with cancellation support
inline coro::task<std::optional<response>> get(std::string_view url, coro::cancel_token token) {
    client c;
    co_return co_await c.get(url, std::move(token));
}

/// Perform HTTP POST request
/// @return Response on success, std::nullopt on error (check errno)
inline coro::task<std::optional<response>> post(std::string_view url,
                                                std::string_view body,
                                                std::string_view content_type = mime::application_form_urlencoded) {
    client c;
    co_return co_await c.post(url, body, content_type);
}

/// Perform HTTP POST request with cancellation support
inline coro::task<std::optional<response>> post(std::string_view url,
                                                std::string_view body,
                                                coro::cancel_token token,
                                                std::string_view content_type = mime::application_form_urlencoded) {
    client c;
    co_return co_await c.post(url, body, std::move(token), content_type);
}

} // namespace elio::http
