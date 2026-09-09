#pragma once

#include <elio/http/http_common.hpp>
#include <elio/http/http_parser.hpp>
#include <elio/http/http_response_reader.hpp>
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
#include <optional>
#include <stdexcept>

namespace elio::http {

#ifdef ELIO_RUNTIME_TEST_HOOKS
namespace detail {
// Expire only the Expect clock after final headers, allowing a regression to
// hold the final body behind a barrier without relying on timer scheduling.
inline std::atomic<bool> expire_expect_after_headers_for_test{false};
inline std::atomic<bool> final_headers_seen_for_test{false};
} // namespace detail
#endif

/// HTTP client configuration
struct client_config : base_client_config {
    size_t max_redirects = 5;                     ///< Max redirects to follow
    bool follow_redirects = true;                 ///< Auto-follow redirects
    size_t max_connections_per_host = 6;          ///< Max connections per host
    std::chrono::seconds pool_idle_timeout{60};   ///< Idle connection timeout
    /// Hard cap on the total bytes a single response may occupy in the
    /// parser. Mirrors server_config::max_request_size on the server side.
    /// A hostile or buggy server can otherwise stream gigabytes through
    /// response_parser and OOM the client. 16 MiB is large enough for typical
    /// API/JSON responses; bump it deliberately for endpoints that legitimately
    /// return larger bodies.
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

    explicit connection_pool(client_config config = {})
        : config_(config) {}
    
    /// Get or create a connection to host
    coro::task<std::optional<connection>> acquire(const std::string& host,
                                                   uint16_t port,
                                                   bool secure,
                                                   tls::tls_context* tls_ctx = nullptr,
                                                   std::chrono::nanoseconds connect_timeout =
                                                       std::chrono::nanoseconds::zero(),
                                                   coro::cancel_token token = {}) {
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

        // Create new connection using client_connect utility
        auto result = co_await client_connect(
            host,
            port,
            secure,
            tls_ctx,
            config_.resolve_options,
            config_.rotate_resolved_addresses,
            connect_timeout,
            std::move(token));
        if (!result) {
            co_return std::nullopt;
        }

        co_return std::move(*result);
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
        if (!detail::is_supported_http_url_scheme(target.scheme)) {
            ELIO_LOG_ERROR("Unsupported HTTP URL scheme: {}", target.scheme);
            errno = EINVAL;
            co_return std::nullopt;
        }
        co_return co_await send_request(req, target, 0, std::move(token));
    }
    
    /// Get TLS context for configuration
    tls::tls_context& tls_context() noexcept { return tls_ctx_; }
    
    /// Get configuration
    client_config& config() noexcept { return config_; }
    const client_config& config() const noexcept { return config_; }
    
private:
    /// Perform request to URL
    coro::task<std::optional<response>> request_url(method m, 
                                                          std::string_view url_str,
                                                          std::string_view body,
                                                          std::string_view content_type,
                                                          coro::cancel_token token) {
        // Check if already cancelled
        if (token.is_cancelled()) {
            errno = ECANCELED;
            co_return std::nullopt;
        }
        
        auto parsed = url::parse(url_str);
        if (!parsed) {
            ELIO_LOG_ERROR("Invalid URL: {}", url_str);
            errno = EINVAL;
            co_return std::nullopt;
        }
        if (!detail::is_supported_http_url_scheme(parsed->scheme)) {
            ELIO_LOG_ERROR("Unsupported HTTP URL scheme: {}", parsed->scheme);
            errno = EINVAL;
            co_return std::nullopt;
        }

        if (!config_.user_agent.empty() &&
            !detail::is_valid_header_value(config_.user_agent)) {
            ELIO_LOG_ERROR("Invalid User-Agent header value");
            errno = EINVAL;
            co_return std::nullopt;
        }
        if (!content_type.empty() &&
            !detail::is_valid_header_value(content_type)) {
            ELIO_LOG_ERROR("Invalid Content-Type header value");
            errno = EINVAL;
            co_return std::nullopt;
        }
        
        request req(m, parsed->path_with_query());
        req.set_host(parsed->host_authority());
        
        if (!body.empty()) {
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
    
    /// Spawn a watchdog that shutdown(2)s `fd` after `timeout` elapses,
    /// mirroring rpc_session::read_frame_with_deadline. The returned
    /// join_handle must be awaited after the IO completes; the caller
    /// cancels `cancel_src` so the watchdog wakes early on success.
    /// `timed_out` is set to true iff the deadline fired before the IO
    /// completed.
    static coro::join_handle<void>
    arm_io_watchdog(runtime::scheduler* sched,
                    int fd,
                    std::chrono::nanoseconds timeout,
                    coro::cancel_token watchdog_token,
                    std::shared_ptr<std::atomic<bool>> timed_out) {
        return sched->go_joinable(
            [fd, timeout, tok = std::move(watchdog_token),
             flag = std::move(timed_out)]() -> coro::task<void> {
                auto r = co_await elio::time::sleep_for(timeout, tok);
                if (r == coro::cancel_result::completed) {
                    flag->store(true, std::memory_order_release);
                    if (fd >= 0) {
                        ::shutdown(fd, SHUT_RDWR);
                    }
                }
                co_return;
            });
    }

    static bool is_informational_status(uint16_t code) noexcept {
        return code >= 100 && code < 200;
    }

    /// Write the whole buffer to the connection, optionally bounded by
    /// `io_deadline`. read_timeout doubles as the send deadline: a stalled
    /// write to a malicious server is the same liveness problem as a
    /// stalled read, so the same bound applies. The watchdog shutdown(2)s
    /// the fd to abort an in-flight write on timeout. Returns true on
    /// success; on failure sets errno (ECANCELED / ETIMEDOUT / connection
    /// error) and returns false.
    static coro::task<bool>
    write_request_data(connection& conn, std::string_view data,
                       const url& target,
                       std::chrono::nanoseconds io_deadline,
                       bool deadline_enforced,
                       runtime::scheduler* sched,
                       const coro::cancel_token& token) {
        io::io_result write_result{};
        if (deadline_enforced) {
            auto timed_out = std::make_shared<std::atomic<bool>>(false);
            coro::cancel_source ws_cancel;
            auto watchdog = arm_io_watchdog(sched, conn.fd(), io_deadline,
                                            ws_cancel.get_token(), timed_out);
            write_result = co_await conn.write_all(data, token);
            ws_cancel.cancel();
            co_await watchdog;
            if (timed_out->load(std::memory_order_acquire)) {
                conn.mark_externally_shut_down();
                ELIO_LOG_ERROR("Write to {}:{} timed out after {}s",
                               target.host, target.effective_port(),
                               std::chrono::duration_cast<std::chrono::seconds>(io_deadline).count());
                errno = ETIMEDOUT;
                co_return false;
            }
        } else {
            write_result = co_await conn.write_all(data, token);
        }
        if (write_result.result <= 0) {
            if (write_result.result == -ECANCELED && token.is_cancelled()) {
                detail::abort_stream_io(conn);
                errno = ECANCELED;
                co_return false;
            }
            ELIO_LOG_ERROR("Failed to send request: {}",
                           write_result.result == 0 ? "connection closed"
                                                    : strerror(-write_result.result));
            errno = write_result.result == 0 ? ECONNRESET : -write_result.result;
            co_return false;
        }
        co_return true;
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

    /// Send request with redirect handling
    coro::task<std::optional<response>> send_request(request& req, const url& target,
                                                           size_t redirect_count,
                                                           coro::cancel_token token) {
        // Check if cancelled
        if (token.is_cancelled()) {
            errno = ECANCELED;
            co_return std::nullopt;
        }

        if (!detail::is_valid_url_input(target.host_authority()) ||
            !detail::is_valid_request_target(req.path_with_query())) {
            ELIO_LOG_ERROR("Invalid outbound HTTP request target");
            errno = EINVAL;
            co_return std::nullopt;
        }

        // Get connection from pool. connect_timeout is enforced inside the
        // shared client_connect path for TCP connect and TLS handshake.
        errno = 0;
        auto conn_opt = co_await pool_.acquire(target.host, target.effective_port(),
                                                target.is_secure(), &tls_ctx_,
                                                config_.connect_timeout, token);
        if (!conn_opt) {
            if (errno == 0) {
                errno = ECONNREFUSED;
            }
            co_return std::nullopt;
        }

        auto& conn = *conn_opt;

        // Ensure Host header is set
        if (req.header("Host").empty()) {
            req.set_host(target.host_authority());
        }

        // Add keep-alive header
        if (!req.get_headers().contains("Connection")) {
            req.set_header("Connection", "keep-alive");
        }

        // Serialize and send request. With Expect: 100-continue and a
        // non-empty body, only the headers go out first; the body is held
        // back until the server answers with an interim 100 Continue, a
        // final response (body suppressed), or the fallback timeout
        // expires (body sent anyway).
        const bool defer_body = req.expect_continue() && !req.body().empty();
        std::string request_data;
        try {
            request_data = defer_body ? req.serialize_headers()
                                      : req.serialize();
        } catch (const std::invalid_argument& ex) {
            ELIO_LOG_ERROR("Invalid outbound HTTP request: {}", ex.what());
            errno = EINVAL;
            co_return std::nullopt;
        }

        ELIO_LOG_DEBUG("Sending request to {}:{}\n{}", target.host, target.effective_port(), request_data);

        // Check cancellation before write
        if (token.is_cancelled()) {
            errno = ECANCELED;
            co_return std::nullopt;
        }

        auto* sched = runtime::scheduler::current();
        const bool deadline_enforced =
            sched != nullptr && config_.read_timeout.count() > 0;
        const auto io_deadline = config_.read_timeout;

        // Compute an absolute deadline for the entire response so that a
        // slowloris-style server cannot reset the timer on every byte.
        const auto response_deadline =
            std::chrono::steady_clock::now() + io_deadline;

        if (!co_await write_request_data(conn, request_data, target,
                                         io_deadline, deadline_enforced,
                                         sched, token)) {
            co_return std::nullopt;
        }

        // One incremental decoder handles both the Expect gate and the final
        // response. Headers-complete is distinct from body/message completion.
        response_reader reader(config_.read_buffer_size);
        reader.set_max_headers(config_.max_headers);
        reader.set_max_header_size(config_.max_header_size);
        reader.set_request_method(req.get_method());
        std::string response_body;
        size_t skipped_informational_bytes = 0;
        bool body_pending = defer_body;
        const bool expect_bounded = sched != nullptr &&
            config_.expect_continue_timeout.count() > 0;
        auto expect_deadline = std::chrono::steady_clock::now() +
            config_.expect_continue_timeout;
        bool expect_expired = false;

        auto send_pending_body = [&]() -> coro::task<bool> {
            if (!body_pending) co_return true;
            body_pending = false;
            const auto remaining = response_deadline - std::chrono::steady_clock::now();
            if (deadline_enforced && remaining <= std::chrono::steady_clock::duration::zero()) {
                errno = ETIMEDOUT;
                co_return false;
            }
            co_return co_await write_request_data(conn, req.body(), target,
                deadline_enforced ? remaining : io_deadline, deadline_enforced, sched, token);
        };
        if (body_pending && !expect_bounded && !co_await send_pending_body()) {
            co_return std::nullopt;
        }

        // Read policy owns only deadlines and transport I/O, never HTTP state.
        // An Expect timeout cancels this read rather than shutting down the fd.
        auto receive = [&](void* data, size_t size) -> coro::task<io::io_result> {
            expect_expired = false;
            const auto now = std::chrono::steady_clock::now();
            if (deadline_enforced && now >= response_deadline) {
                co_return io::io_result{-ETIMEDOUT, 0};
            }
            const bool waiting_expect = body_pending && expect_bounded;
            if (waiting_expect && now >= expect_deadline) {
                expect_expired = true;
                co_return io::io_result{-ETIMEDOUT, 0};
            }
#ifdef ELIO_RUNTIME_TEST_HOOKS
            detail::arm_client_response_read_observer_for_test();
#endif
            if (!deadline_enforced && !waiting_expect) {
                co_return co_await conn.read(data, size, token);
            }
            const bool expect_first = waiting_expect &&
                (!deadline_enforced || expect_deadline < response_deadline);
            const auto deadline = expect_first ? expect_deadline : response_deadline;
            auto read_cancel = std::make_shared<coro::cancel_source>();
            auto forward = token.on_cancel([read_cancel] { read_cancel->cancel(); });
            auto expired = std::make_shared<std::atomic<bool>>(false);
            coro::cancel_source watchdog_cancel;
            auto watchdog = sched->go_joinable(
                [deadline, expired, read_cancel,
                 stop = watchdog_cancel.get_token()]() -> coro::task<void> {
                    auto result = co_await elio::time::sleep_for(
                        deadline - std::chrono::steady_clock::now(), stop);
                    if (result == coro::cancel_result::completed) {
                        expired->store(true, std::memory_order_release);
                        read_cancel->cancel();
                    }
                });
            auto result = co_await conn.read(data, size, read_cancel->get_token());
            watchdog_cancel.cancel();
            co_await watchdog;
            if (expired->load(std::memory_order_acquire)) {
                // Preserve bytes read concurrently with the Expect timeout:
                // they might contain final headers that suppress the upload.
                if (expect_first && result.result > 0) co_return result;
                expect_expired = expect_first;
                co_return io::io_result{-ETIMEDOUT, 0};
            }
            co_return result;
        };

        while (true) {
            if (token.is_cancelled()) {
                errno = ECANCELED;
                co_return std::nullopt;
            }
            auto part = co_await reader.read_with(receive, token);
            if (!part.success()) {
                if (expect_expired && body_pending && !token.is_cancelled()) {
                    if (!co_await send_pending_body()) co_return std::nullopt;
                    continue;
                }
                errno = part.error;
                co_return std::nullopt;
            }
            const auto code = reader.decoder().status_code();
            if (part.event == response_event::headers_complete) {
#ifdef ELIO_RUNTIME_TEST_HOOKS
                if (!is_informational_status(code) &&
                    detail::expire_expect_after_headers_for_test.load(std::memory_order_acquire)) {
                    expect_deadline = std::chrono::steady_clock::now();
                    detail::client_response_read_staged_for_test.store(false, std::memory_order_release);
                    detail::final_headers_seen_for_test.store(true, std::memory_order_release);
                }
#endif
                if (code == static_cast<uint16_t>(status::switching_protocols)) {
                    errno = EBADMSG;
                    co_return std::nullopt;
                }
                if (!is_informational_status(code)) {
                    body_pending = false; // Final headers suppress the upload.
                } else if (code == static_cast<uint16_t>(status::continue_)) {
                    if (!co_await send_pending_body()) co_return std::nullopt;
                }
            } else if (part.event == response_event::body) {
                if (part.body.size() > config_.max_response_size -
                    std::min(response_body.size(), config_.max_response_size)) {
                    errno = EMSGSIZE;
                    co_return std::nullopt;
                }
                response_body.append(part.body);
            } else if (part.event == response_event::protocol_handoff) {
                // Ordinary clients never transfer ownership to a tunnel.
                errno = EBADMSG;
                co_return std::nullopt;
            } else if (part.event == response_event::message_complete) {
                if (!is_informational_status(code)) break;
                if (reader.message_bytes() > config_.max_response_size -
                    skipped_informational_bytes) {
                    errno = EMSGSIZE;
                    co_return std::nullopt;
                }
                skipped_informational_bytes += reader.message_bytes();
                if (!reader.next_response()) {
                    errno = EBADMSG;
                    co_return std::nullopt;
                }
                reader.set_request_method(req.get_method());
            }
        }
        auto resp = response::from_decoder(reader.decoder(), std::move(response_body));

        // Return connection to pool only when (a) keep-alive is allowed by
        // the response, (b) the parser is in the complete state, and (c) no
        // bytes are left over in the parser's input buffer. A non-empty
        // remaining buffer means the server pipelined extra bytes after the
        // response — pooling the conn would let those bytes be misread as the
        // head of the next response (response-splitting). On any failure of
        // these conditions the connection is simply dropped on scope exit.
        if (reader.decoder().is_complete() &&
            !reader.decoder().is_close_delimited() &&
            !reader.reached_eof() &&
            reader.bytes_remaining() == 0 &&
            reader.decoder().get_headers().keep_alive(reader.decoder().version())) {
            pool_.release(target.host, target.effective_port(), target.is_secure(), std::move(conn));
        }
        
        // Handle redirects
        if (config_.follow_redirects &&
            is_auto_follow_redirect(resp.get_status()) &&
            redirect_count < config_.max_redirects) {
            auto location = resp.header("Location");
            if (!location.empty()) {
                ELIO_LOG_DEBUG("Following redirect to: {}", location);

                if (!detail::is_valid_url_input(location)) {
                    ELIO_LOG_WARNING("Rejecting invalid redirect Location: {}",
                                     location);
                    co_return resp;
                }
                
                auto redirect_url = url::resolve_reference(target, location);
                
                if (redirect_url) {
                    if (!detail::is_supported_http_url_scheme(redirect_url->scheme)) {
                        ELIO_LOG_WARNING("Rejecting unsupported redirect scheme: {}",
                                         redirect_url->scheme);
                        co_return resp;
                    }

                    // Reject HTTPS -> HTTP downgrades to prevent SSL stripping
                    if (target.is_secure() && !redirect_url->is_secure()) {
                        ELIO_LOG_WARNING("Rejecting insecure redirect from HTTPS to HTTP: {}",
                                         location);
                        co_return resp;
                    }

                    // Change method to GET for 303, except HEAD must remain
                    // HEAD so its response-body semantics are preserved.
                    method redirect_method = req.get_method();
                    if ((resp.get_status() == status::see_other &&
                         req.get_method() != method::HEAD) ||
                        ((resp.get_status() == status::moved_permanently || 
                          resp.get_status() == status::found) && 
                         req.get_method() == method::POST)) {
                        redirect_method = method::GET;
                    }
                    
                    request redirect_req(redirect_method, redirect_url->path_with_query());
                    redirect_req.set_host(redirect_url->host_authority());
                    if (!config_.user_agent.empty()) {
                        redirect_req.set_header("User-Agent", config_.user_agent);
                    }
                    
                    // Keep the payload whenever redirect handling preserves
                    // the original method. 303 intentionally switches to GET.
                    const bool method_preserved =
                        redirect_method == req.get_method() &&
                        resp.get_status() != status::see_other;
                    if ((resp.get_status() == status::temporary_redirect ||
                         resp.get_status() == status::permanent_redirect ||
                         method_preserved) &&
                        !req.body().empty()) {
                        redirect_req.set_body(req.body());
                        auto ct = req.content_type();
                        if (!ct.empty()) {
                            redirect_req.set_content_type(ct);
                        }
                        // The Expect handshake only makes sense while the
                        // body travels with the redirect; it re-runs per hop.
                        if (req.expect_continue()) {
                            redirect_req.set_expect_continue();
                        }
                    }
                    
                    co_return co_await send_request(redirect_req, *redirect_url, redirect_count + 1, token);
                }
            }
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
