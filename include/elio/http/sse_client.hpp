#pragma once

/// @file sse_client.hpp
/// @brief Server-Sent Events (SSE) client implementation for Elio
///
/// This file provides SSE client functionality including:
/// - Connection to SSE endpoints (http:// and https://)
/// - Event parsing and delivery
/// - Automatic reconnection support
/// - Last-Event-ID tracking

#include <elio/http/sse_server.hpp>
#include <elio/http/http_common.hpp>
#include <elio/http/http_parser.hpp>
#include <elio/http/client_base.hpp>
#include <elio/net/stream.hpp>
#include <elio/io/io_context.hpp>
#include <elio/coro/task.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/time/timer.hpp>
#include <elio/log/macros.hpp>

#include <string>
#include <string_view>
#include <cerrno>
#include <memory>
#include <optional>
#include <vector>
#include <functional>

namespace elio::http::sse {

/// SSE client configuration
struct client_config : http::base_client_config {
    int default_retry_ms = 3000;                  ///< Default reconnect interval
    bool auto_reconnect = true;                   ///< Enable auto-reconnection
    size_t max_reconnect_attempts = 0;            ///< Max reconnect attempts (0 = unlimited)
    size_t max_event_buffer_size = 1024 * 1024;   ///< Max pending SSE line/event buffer bytes
    std::string last_event_id;                    ///< Initial Last-Event-ID

    client_config() {
        user_agent = "elio-sse-client/1.0";
        read_buffer_size = 4096;  // SSE uses smaller buffer
    }
};

/// SSE connection state
enum class client_state {
    disconnected,  ///< Not connected
    connecting,    ///< Connection in progress
    connected,     ///< Connected and receiving events
    reconnecting,  ///< Reconnecting after disconnect
    closed         ///< Permanently closed
};

/// SSE event parser
class event_parser {
public:
    static constexpr size_t default_max_buffer_size = 1024 * 1024;

    explicit event_parser(size_t max_buffer_size = default_max_buffer_size) noexcept
        : max_buffer_size_(max_buffer_size) {}

    event_parser(size_t max_buffer_size, std::string_view last_event_id)
        : last_event_id_(last_event_id)
        , max_buffer_size_(max_buffer_size) {}
    
    /// Parse incoming data and extract events
    /// @param data Input data
    /// @return Number of events parsed
    size_t parse(std::string_view data) {
        if (failed_) {
            return 0;
        }

        size_t events_found = 0;
        if (skip_lf_after_terminal_cr_ && !data.empty()) {
            if (data.front() == '\n') {
                data.remove_prefix(1);
            }
            skip_lf_after_terminal_cr_ = false;
        }

        while (!data.empty() && !failed_) {
            auto lf_pos = data.find('\n');
            auto cr_pos = data.find('\r');

            size_t line_end = std::string::npos;
            size_t consume = 0;
            bool terminal_cr_at_input_end = false;
            if (lf_pos == std::string::npos && cr_pos == std::string::npos) {
                if (append_would_exceed_limit(buffer_.size(), data.size(), 0)) {
                    fail("SSE line exceeds configured buffer limit");
                    break;
                }
                buffer_.append(data);
                break;
            } else if (cr_pos != std::string::npos &&
                       (lf_pos == std::string::npos || cr_pos < lf_pos)) {
                line_end = cr_pos;
                consume = (cr_pos + 1 < data.size() && data[cr_pos + 1] == '\n')
                        ? cr_pos + 2
                        : cr_pos + 1;
                terminal_cr_at_input_end = consume == data.size() &&
                                           data[consume - 1] == '\r';
            } else {
                line_end = lf_pos;
                consume = lf_pos + 1;
            }

            // Limit applies to line content. The 1-2 terminator bytes are
            // appended with the line and immediately consumed by process_buffer().
            if (append_would_exceed_limit(buffer_.size(), line_end, 0)) {
                fail("SSE line exceeds configured buffer limit");
                break;
            }
            buffer_.append(data.substr(0, consume));
            data.remove_prefix(consume);
            events_found += process_buffer(terminal_cr_at_input_end);
        }

        return failed_ ? 0 : events_found;
    }

    /// True after an input line or in-progress event exceeds the configured limit.
    bool failed() const noexcept { return failed_; }

    /// Human-readable parser failure reason, empty if parsing has not failed.
    std::string_view error_message() const noexcept { return error_message_; }
    
    /// Check if an event is available
    bool has_event() const { return !events_.empty(); }
    
    /// Get next event (removes from queue)
    std::optional<event> get_event() {
        if (events_.empty()) return std::nullopt;
        auto evt = std::move(events_.front());
        events_.erase(events_.begin());
        return evt;
    }
    
    /// Get last event ID
    std::string_view last_event_id() const { return last_event_id_; }
    
    /// Get current retry interval
    int retry_ms() const { return retry_ms_; }
    
    /// Reset parser state
    void reset() {
        buffer_.clear();
        current_event_ = event{};
        current_event_has_id_ = false;
        events_.clear();
        failed_ = false;
        error_message_.clear();
        skip_lf_after_terminal_cr_ = false;
        // Don't reset last_event_id_ or retry_ms_ - these persist
    }
    
private:
    size_t process_buffer(bool terminal_cr_at_input_end) {
        size_t events_found = 0;

        while (true) {
            // SSE spec (HTML Living Standard §9.2.6): lines may be terminated
            // by \n, \r\n, or \r alone.  Find the earliest line terminator.
            auto lf_pos = buffer_.find('\n');
            auto cr_pos = buffer_.find('\r');

            size_t line_end = std::string::npos;
            size_t consume = 0;  // bytes to erase including terminator

            if (lf_pos == std::string::npos && cr_pos == std::string::npos) {
                // parse() enforces the partial-line limit before appending
                // unterminated input. No complete line is available yet.
                break;  // no line terminator found
            } else if (cr_pos != std::string::npos &&
                       (lf_pos == std::string::npos || cr_pos < lf_pos)) {
                // \r found first — could be standalone \r or start of \r\n
                line_end = cr_pos;
                if (cr_pos + 1 < buffer_.size()) {
                    if (buffer_[cr_pos + 1] == '\n') {
                        consume = cr_pos + 2;  // consume \r\n together
                    } else {
                        consume = cr_pos + 1;  // standalone \r
                    }
                } else {
                    // \r at end of buffer is provisionally processed as
                    // standalone only when it also ended the caller's input
                    // chunk.  A leading \n in the next chunk then belongs to
                    // this same CRLF terminator and must not create an extra
                    // blank line.
                    consume = cr_pos + 1;
                    skip_lf_after_terminal_cr_ = terminal_cr_at_input_end;
                }
            } else {
                // \n found first (no preceding \r — that case is handled above)
                line_end = lf_pos;
                consume = lf_pos + 1;
            }

            if (line_end > max_buffer_size_) {
                fail("SSE line exceeds configured buffer limit");
                break;
            }

            std::string line = buffer_.substr(0, line_end);
            buffer_.erase(0, consume);

            if (line.empty()) {
                // Empty line = dispatch event.  Per the SSE spec (HTML Living
                // Standard §9.2.6), an event is dispatched ONLY when the
                // data buffer is non-empty.  Events carrying only `event:`
                // (no data) reset state without producing a user-visible
                // event.
                if (current_event_has_id_) {
                    last_event_id_ = current_event_.id;
                }

                if (!current_event_.data.empty()) {
                    // Remove trailing newline from data
                    if (current_event_.data.back() == '\n') {
                        current_event_.data.pop_back();
                    }

                    events_.push_back(std::move(current_event_));
                    ++events_found;
                }
                // Always reset state on blank line (spec requirement)
                current_event_ = event{};
                current_event_has_id_ = false;
                continue;
            }
            
            // Check for comment
            if (line[0] == ':') {
                // Comment line, ignore
                continue;
            }
            
            // Parse field:value
            auto colon_pos = line.find(':');
            std::string field;
            std::string value;
            
            if (colon_pos == std::string::npos) {
                field = line;
                value = "";
            } else {
                field = line.substr(0, colon_pos);
                value = line.substr(colon_pos + 1);
                // Remove leading space from value
                if (!value.empty() && value[0] == ' ') {
                    value.erase(0, 1);
                }
            }
            
            // Process field
            if (field == "event") {
                current_event_.type = value;
            } else if (field == "data") {
                if (append_would_exceed_limit(current_event_.data.size(),
                                              value.size(), 1)) {
                    fail("SSE event data exceeds configured buffer limit");
                    break;
                }
                current_event_.data += value;
                current_event_.data += '\n';
            } else if (field == "id") {
                // ID cannot contain null
                if (value.find('\0') == std::string::npos) {
                    current_event_.id = value;
                    current_event_has_id_ = true;
                }
            } else if (field == "retry") {
                // Parse retry as integer with overflow protection.
                // Clamp to a sane maximum (24 hours = 86400000 ms) to prevent
                // unreasonably long reconnect intervals.
                int64_t retry = 0;
                bool valid = true;
                for (char c : value) {
                    if (c >= '0' && c <= '9') {
                        retry = retry * 10 + (c - '0');
                        if (retry > 86400000LL) {
                            retry = 86400000LL;
                            break;
                        }
                    } else {
                        valid = false;
                        break;
                    }
                }
                if (valid && !value.empty()) {
                    retry_ms_ = static_cast<int>(retry);
                }
            }
            // Ignore unknown fields
        }
        
        return events_found;
    }

    bool append_would_exceed_limit(size_t current,
                                   size_t value_size,
                                   size_t extra) const noexcept {
        if (value_size > max_buffer_size_) {
            return true;
        }
        const size_t append_size = value_size + extra;
        if (append_size < value_size || append_size > max_buffer_size_) {
            return true;
        }
        return current > max_buffer_size_ - append_size;
    }

    void fail(std::string_view message) {
        failed_ = true;
        error_message_ = std::string(message);
        buffer_.clear();
        current_event_ = event{};
        current_event_has_id_ = false;
        events_.clear();
        skip_lf_after_terminal_cr_ = false;
    }
    
    std::string buffer_;
    event current_event_;
    bool current_event_has_id_ = false;
    std::vector<event> events_;
    std::string last_event_id_;
    int retry_ms_ = 3000;
    size_t max_buffer_size_ = default_max_buffer_size;
    bool failed_ = false;
    bool skip_lf_after_terminal_cr_ = false;
    std::string error_message_;
};

/// SSE client
class sse_client {
public:
    /// Create SSE client with default configuration
    sse_client() : sse_client(client_config{}) {}

    /// Create SSE client with configuration
    explicit sse_client(client_config config)
        : config_(config)
        , tls_ctx_(tls::tls_mode::client)
        , last_event_id_(config.last_event_id)
        , parser_(config.max_event_buffer_size, config.last_event_id) {
        // Setup TLS context using shared utility
        http::init_client_tls_context(tls_ctx_, config_.verify_certificate);

        buffer_.resize(config_.read_buffer_size);
    }
    
    /// Destructor
    ~sse_client() = default;
    
    // Move only
    sse_client(sse_client&&) = default;
    sse_client& operator=(sse_client&&) = default;
    sse_client(const sse_client&) = delete;
    sse_client& operator=(const sse_client&) = delete;
    
    /// Connect to an SSE endpoint
    /// @param url HTTP(S) URL
    /// @return true on success
    coro::task<bool> connect(std::string_view url_str) {
        return connect_impl(url_str, coro::cancel_token{});
    }
    
    /// Connect to an SSE endpoint with cancellation support
    /// @param url HTTP(S) URL
    /// @param token Cancellation token
    /// @return true on success
    coro::task<bool> connect(std::string_view url_str, coro::cancel_token token) {
        return connect_impl(url_str, std::move(token));
    }
    
    /// Get connection state
    client_state state() const noexcept { return state_; }
    
    /// Check if connected
    bool is_connected() const noexcept { return state_ == client_state::connected; }
    
    /// Get last event ID
    std::string_view last_event_id() const noexcept { return last_event_id_; }
    
    /// Receive next event (blocks until event available or connection closed)
    coro::task<std::optional<event>> receive() {
        return receive_impl(coro::cancel_token{});
    }
    
    /// Receive next event with cancellation support
    /// @param token Cancellation token
    /// @return Event on success, std::nullopt on close/error/cancel
    coro::task<std::optional<event>> receive(coro::cancel_token token) {
        return receive_impl(std::move(token));
    }
    
    /// Close the connection
    coro::task<void> close() {
        state_ = client_state::closed;
        co_await stream_.close();
    }
    
    /// Get TLS context for configuration
    tls::tls_context& tls_context() noexcept { return tls_ctx_; }
    
    /// Get configuration
    client_config& config() noexcept { return config_; }
    const client_config& config() const noexcept { return config_; }
    
private:
    /// Internal connect implementation
    coro::task<bool> connect_impl(std::string_view url_str, coro::cancel_token token) {
        // Check if already cancelled
        if (token.is_cancelled()) {
            errno = ECANCELED;
            co_return false;
        }
        
        // Parse URL
        auto parsed = url::parse(url_str);
        if (!parsed) {
            ELIO_LOG_ERROR("Invalid SSE URL: {}", url_str);
            errno = EINVAL;
            co_return false;
        }
        if (!http::detail::is_supported_http_url_scheme(parsed->scheme)) {
            ELIO_LOG_ERROR("Unsupported SSE URL scheme: {}", parsed->scheme);
            errno = EINVAL;
            co_return false;
        }
        
        url_ = *parsed;
        token_ = std::move(token);
        state_ = client_state::connecting;
        
        co_return co_await do_connect();
    }
    
    /// Internal receive implementation
    coro::task<std::optional<event>> receive_impl(coro::cancel_token token) {
        // Observe BOTH tokens at every loop point.  Either the parameter
        // token (per-receive cancellation) or the connect-time token must be
        // able to break out of the loop; the previous `auto& active_token =
        // token.is_cancelled() ? token_ : token;` selected exactly the wrong
        // one and silently dropped the connect-time token's cancellation.
        auto cancelled = [&] {
            return token.is_cancelled() || token_.is_cancelled();
        };

        while (state_ == client_state::connected) {
            // Check for cancellation
            if (cancelled()) {
                errno = ECANCELED;
                co_return std::nullopt;
            }

            // Check for already-parsed events
            if (parser_.has_event()) {
                auto evt = parser_.get_event();
                co_return evt;
            }

            // Read more data. Either the per-receive token or the
            // connect-time token must be able to cancel the pending read.
            auto read_cancel = std::make_shared<coro::cancel_source>();
            auto propagate_cancel =
                [read_cancel]() { read_cancel->cancel(); };
            auto receive_cancel_registration = token.on_cancel(propagate_cancel);
            auto connection_cancel_registration =
                token_.on_cancel(std::move(propagate_cancel));
            if (cancelled()) {
                read_cancel->cancel();
            }
            auto result = co_await read(buffer_.data(), buffer_.size(),
                                        read_cancel->get_token());

            if (result.result <= 0) {
                if (result.result == -ECANCELED && cancelled()) {
                    errno = ECANCELED;
                    co_return std::nullopt;
                }
                if (result.result == 0) {
                    ELIO_LOG_DEBUG("SSE connection closed by server");
                } else {
                    ELIO_LOG_ERROR("SSE read error: {}", strerror(-result.result));
                }

                // Check cancellation before reconnect
                if (cancelled()) {
                    state_ = client_state::disconnected;
                    co_return std::nullopt;
                }

                // Handle reconnection
                if (config_.auto_reconnect && state_ != client_state::closed) {
                    state_ = client_state::reconnecting;
                    bool reconnected = co_await try_reconnect();
                    if (reconnected) {
                        continue;
                    }
                }

                state_ = client_state::disconnected;
                co_return std::nullopt;
            }

            parser_.parse(std::string_view(buffer_.data(),
                                           static_cast<size_t>(result.result)));
            if (parser_.failed()) {
                ELIO_LOG_ERROR("SSE parse error: {}", parser_.error_message());
                state_ = client_state::disconnected;
                co_return std::nullopt;
            }
            sync_last_event_id_from_parser();
        }

        co_return std::nullopt;
    }

    coro::task<bool> do_connect() {
        ELIO_LOG_DEBUG("Connecting to SSE endpoint {}:{}{}",
                      url_.host, url_.effective_port(), url_.path);

        if (!config_.user_agent.empty() &&
            !http::detail::is_valid_header_value(config_.user_agent)) {
            ELIO_LOG_ERROR("Invalid SSE User-Agent header value");
            errno = EINVAL;
            state_ = client_state::disconnected;
            co_return false;
        }
        if (!last_event_id_.empty() &&
            !http::detail::is_valid_header_value(last_event_id_)) {
            ELIO_LOG_ERROR("Invalid SSE Last-Event-ID header value");
            errno = EINVAL;
            state_ = client_state::disconnected;
            co_return false;
        }

        // Establish connection using shared utility
        auto conn_result = co_await http::client_connect(
            url_.host,
            url_.effective_port(),
            url_.is_secure(),
            &tls_ctx_,
            config_.resolve_options,
            config_.rotate_resolved_addresses,
            config_.connect_timeout,
            token_);
        if (!conn_result) {
            state_ = client_state::disconnected;
            co_return false;
        }
        stream_ = std::move(*conn_result);

        auto fail_connect = [&]() noexcept {
            int saved_errno = errno;
            stream_.disconnect();
            errno = saved_errno;
            state_ = client_state::disconnected;
            return false;
        };
        
        // Send HTTP request
        std::string request;
        request += "GET ";
        request += url_.path_with_query();
        request += " HTTP/1.1\r\n";
        request += "Host: ";
        request += url_.host_authority();
        request += "\r\n";
        request += "Accept: text/event-stream\r\n";
        request += "Cache-Control: no-cache\r\n";
        
        if (!config_.user_agent.empty()) {
            request += "User-Agent: ";
            request += config_.user_agent;
            request += "\r\n";
        }
        
        if (!last_event_id_.empty()) {
            request += "Last-Event-ID: ";
            request += last_event_id_;
            request += "\r\n";
        }
        
        request += "\r\n";
        
        auto send_result = co_await write_exactly(request.data(), request.size(),
                                                  token_);
        if (send_result.result != static_cast<ssize_t>(request.size())) {
            if (send_result.result == -ECANCELED && token_.is_cancelled()) {
                http::detail::abort_stream_io(stream_);
                errno = ECANCELED;
                co_return fail_connect();
            }
            ELIO_LOG_ERROR("Failed to send SSE request");
            errno = send_result.result == 0 ? ECONNRESET : -send_result.result;
            co_return fail_connect();
        }
        
        // Read response headers.
        //
        // SSE responses MUST be parsed headers-only.  Some misbehaving
        // proxies wrap an SSE stream in a `Content-Length: N` or
        // `Transfer-Encoding: chunked` envelope; if we let the generic
        // `response_parser` consume the stream, it eats real SSE event
        // bytes as the HTTP "body" and the first events vanish into
        // `parser.take_body()` — `response_data.substr(consumed)` would
        // then drop the bytes we just lost.  Instead, delimit the header
        // block manually with `\r\n\r\n` and feed every byte after the
        // delimiter directly to the SSE event parser.
        std::string response_data;
        response_data.reserve(1024);
        auto* sched = runtime::scheduler::current();
        const bool deadline_enforced =
            sched != nullptr && config_.read_timeout.count() > 0;
        const auto response_deadline =
            std::chrono::steady_clock::now() + config_.read_timeout;

        while (true) {
            if (token_.is_cancelled()) {
                errno = ECANCELED;
                co_return fail_connect();
            }

            io::io_result read_result{};
#ifdef ELIO_RUNTIME_TEST_HOOKS
            http::detail::arm_client_response_read_observer_for_test();
#endif
            if (deadline_enforced) {
                auto remaining =
                    response_deadline - std::chrono::steady_clock::now();
                if (remaining.count() <= 0) {
                    ELIO_LOG_ERROR("SSE response headers timed out after {}s",
                                   config_.read_timeout.count());
                    errno = ETIMEDOUT;
                    co_return fail_connect();
                }

                auto timed_out = std::make_shared<std::atomic<bool>>(false);
                coro::cancel_source watchdog_cancel;
                auto watchdog = http::detail::arm_fd_shutdown_watchdog(
                    sched, stream_.fd(), remaining,
                    watchdog_cancel.get_token(), timed_out);
                read_result = co_await read(buffer_.data(), buffer_.size(),
                                            token_);
                watchdog_cancel.cancel();
                co_await watchdog;
                if (timed_out->load(std::memory_order_acquire)) {
                    stream_.mark_externally_shut_down();
                    ELIO_LOG_ERROR("SSE response headers timed out after {}s",
                                   config_.read_timeout.count());
                    errno = ETIMEDOUT;
                    co_return fail_connect();
                }
            } else {
                read_result = co_await read(buffer_.data(), buffer_.size(),
                                            token_);
            }

            if (read_result.result <= 0) {
                if (read_result.result == -ECANCELED && token_.is_cancelled()) {
                    http::detail::abort_stream_io(stream_);
                    errno = ECANCELED;
                    co_return fail_connect();
                }
                ELIO_LOG_ERROR("Failed to read SSE response");
                errno = read_result.result == 0 ? ECONNRESET : -read_result.result;
                co_return fail_connect();
            }

            response_data.append(buffer_.data(), static_cast<size_t>(read_result.result));

            if (::elio::http::detail::response_header_limits_exceeded(
                    response_data, config_.max_headers,
                    config_.max_header_size)) {
                ELIO_LOG_ERROR("SSE response headers too large");
                errno = EMSGSIZE;
                co_return fail_connect();
            }

            auto header_end = response_data.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                size_t header_block_size = header_end + 4;

                // Parse only the header block (status line + headers + CRLF
                // CRLF terminator).  If the server advertises a body via
                // Content-Length / Transfer-Encoding we deliberately ignore
                // it — feeding only the header section keeps response_parser
                // from consuming any SSE event bytes.  The parser will
                // typically return need_more (it expected a body), but
                // status_ and headers_ are already populated by then.
                response_parser parser;
                parser.set_max_headers(config_.max_headers);
                parser.set_max_header_size(config_.max_header_size);
                auto [result, consumed] = parser.parse(
                    std::string_view(response_data).substr(0, header_block_size));
                (void)consumed;

                if (result == parse_result::error) {
                    ELIO_LOG_ERROR("Failed to parse SSE response: {}",
                                   parser.error_message());
                    errno = EBADMSG;
                    co_return fail_connect();
                }

                // Check status code
                if (parser.get_status() != status::ok) {
                    ELIO_LOG_ERROR("SSE request failed: {}",
                                  static_cast<int>(parser.get_status()));
                    errno = EBADMSG;
                    co_return fail_connect();
                }

                // Check content type
                auto content_type = parser.get_headers().get("Content-Type");
                std::string_view media_type = content_type;
                if (auto semicolon = media_type.find(';');
                    semicolon != std::string_view::npos) {
                    media_type = media_type.substr(0, semicolon);
                }
                media_type = http::detail::trim_ows(media_type);
                if (!http::detail::ascii_iequals(media_type, SSE_CONTENT_TYPE)) {
                    ELIO_LOG_ERROR("Unexpected SSE Content-Type: {}", content_type);
                    errno = EBADMSG;
                    co_return fail_connect();
                }

                // Anything past the header delimiter is SSE event data,
                // regardless of what the server claimed about the body.
                if (header_block_size < response_data.size()) {
                    parser_.parse(std::string_view(response_data)
                                      .substr(header_block_size));
                    if (parser_.failed()) {
                        ELIO_LOG_ERROR("SSE parse error: {}",
                                       parser_.error_message());
                        errno = EBADMSG;
                        co_return fail_connect();
                    }
                    sync_last_event_id_from_parser();
                }

                break;
            }
        }
        
        state_ = client_state::connected;
        // Reset backoff on successful connection so the next disconnect
        // starts from the server-advertised or configured default.
        current_retry_ms_ = 0;
        ELIO_LOG_DEBUG("SSE connected to {}{}", url_.host, url_.path);
        co_return true;
    }
    
    coro::task<bool> try_reconnect() {
        // Reset parser but keep last_event_id
        parser_.reset();

        // Close current connection
        co_await stream_.close();

        // Use the persisted backoff value if available, otherwise start from
        // the server-advertised or configured default.  This ensures
        // exponential backoff survives across reconnection cycles (flapping).
        int retry_ms = current_retry_ms_;
        if (retry_ms <= 0) {
            retry_ms = parser_.retry_ms();
            if (retry_ms <= 0) {
                retry_ms = config_.default_retry_ms;
            }
            current_retry_ms_ = retry_ms;
        }
        
        size_t attempts = 0;
        while (state_ == client_state::reconnecting) {
            // Check for cancellation
            if (token_.is_cancelled()) {
                co_return false;
            }
            
            ++attempts;
            
            if (config_.max_reconnect_attempts > 0 && 
                attempts > config_.max_reconnect_attempts) {
                ELIO_LOG_ERROR("SSE max reconnect attempts exceeded");
                co_return false;
            }
            
            ELIO_LOG_DEBUG("SSE reconnecting (attempt {}) in {}ms...", attempts, retry_ms);
            
            // Wait before reconnecting (cancellable)
            auto result = co_await elio::time::sleep_for(
                std::chrono::milliseconds(retry_ms), token_);
            if (result == coro::cancel_result::cancelled) {
                co_return false;
            }
            
            if (state_ != client_state::reconnecting) {
                co_return false;
            }
            
            // Try to connect
            state_ = client_state::connecting;
            if (co_await do_connect()) {
                co_return true;
            }
            
            state_ = client_state::reconnecting;
            
            // Increase retry interval (exponential backoff, max 1 minute)
            // Cap before multiplying to prevent signed integer overflow (UB).
            current_retry_ms_ = std::min(current_retry_ms_, 30000) * 2;
            current_retry_ms_ = std::min(current_retry_ms_, 60000);
            retry_ms = current_retry_ms_;
        }
        
        co_return false;
    }

    void sync_last_event_id_from_parser() {
        auto id = parser_.last_event_id();
        last_event_id_.assign(id.begin(), id.end());
    }
    
    coro::task<io::io_result> read(void* buf, size_t len) {
        co_return co_await stream_.read(buf, len);
    }

    coro::task<io::io_result> read(void* buf, size_t len,
                                   coro::cancel_token token) {
        co_return co_await stream_.read(buf, len, std::move(token));
    }

    coro::task<io::io_result> write(const void* buf, size_t len) {
        co_return co_await stream_.write(buf, len);
    }

    coro::task<io::io_result> write_exactly(const void* buf, size_t len) {
        co_return co_await stream_.write_exactly(buf, len);
    }

    coro::task<io::io_result> write_exactly(const void* buf, size_t len,
                                            coro::cancel_token token) {
        co_return co_await stream_.write_exactly(buf, len, std::move(token));
    }
    
    client_config config_;
    tls::tls_context tls_ctx_;
    net::stream stream_;
    coro::cancel_token token_;  ///< Cancellation token for connection
    
    url url_;
    std::string last_event_id_;
    client_state state_ = client_state::disconnected;
    event_parser parser_;
    std::vector<char> buffer_;
    int current_retry_ms_ = 0;  ///< Persisted backoff value across reconnection cycles
};

/// Convenience function for one-off SSE connection
inline coro::task<std::optional<sse_client>>
sse_connect(std::string_view url, client_config config = {}) {
    auto client = std::make_optional<sse_client>(config);
    bool success = co_await client->connect(url);
    if (!success) {
        co_return std::nullopt;
    }
    co_return client;
}

/// Convenience function for one-off SSE connection with cancellation support
inline coro::task<std::optional<sse_client>>
sse_connect(std::string_view url, coro::cancel_token token, client_config config = {}) {
    auto client = std::make_optional<sse_client>(config);
    bool success = co_await client->connect(url, std::move(token));
    if (!success) {
        co_return std::nullopt;
    }
    co_return client;
}

} // namespace elio::http::sse
