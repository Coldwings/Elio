#pragma once

#include <elio/http/http_common.hpp>
#include <elio/http/http_message.hpp>
#include <elio/tls/tls_stream.hpp>
#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <cerrno>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <queue>
#include <optional>
#include <cstring>
#include <limits>
#include <utility>

namespace elio::http {

/// HTTP/2 error codes
enum class h2_error {
    none = 0,
    protocol_error = NGHTTP2_PROTOCOL_ERROR,
    internal_error = NGHTTP2_INTERNAL_ERROR,
    flow_control_error = NGHTTP2_FLOW_CONTROL_ERROR,
    settings_timeout = NGHTTP2_SETTINGS_TIMEOUT,
    stream_closed = NGHTTP2_STREAM_CLOSED,
    frame_size_error = NGHTTP2_FRAME_SIZE_ERROR,
    refused_stream = NGHTTP2_REFUSED_STREAM,
    cancel = NGHTTP2_CANCEL,
    compression_error = NGHTTP2_COMPRESSION_ERROR,
    connect_error = NGHTTP2_CONNECT_ERROR,
    enhance_your_calm = NGHTTP2_ENHANCE_YOUR_CALM,
    inadequate_security = NGHTTP2_INADEQUATE_SECURITY,
    http_1_1_required = NGHTTP2_HTTP_1_1_REQUIRED
};

/// HTTP/2 stream state
struct h2_stream {
    int32_t stream_id = -1;
    headers response_headers;
    std::string response_body;
    status response_status = status::ok;
    bool headers_complete = false;
    bool body_complete = false;
    bool closed = false;
    h2_error error = h2_error::none;
    bool response_size_exceeded = false;
    
    bool is_complete() const noexcept {
        return headers_complete && body_complete;
    }

    bool append_response_data(std::string_view data,
                              size_t max_response_size) {
        if (error != h2_error::none) {
            return false;
        }

        const auto current = response_body.size();
        if (data.size() > max_response_size ||
            current > max_response_size - data.size()) {
            response_size_exceeded = true;
            error = h2_error::cancel;
            body_complete = true;
            closed = true;
            return false;
        }

        response_body.append(data);
        return true;
    }
};

namespace detail {

inline response materialize_h2_response(h2_stream& stream) {
    response resp(stream.response_status);
    resp.set_body(std::move(stream.response_body));
    resp.get_headers() = std::move(stream.response_headers);
    return resp;
}

} // namespace detail

/// Configuration used when creating an HTTP/2 session.
struct h2_session_config {
    size_t max_concurrent_streams = 100;
    uint32_t initial_window_size = NGHTTP2_INITIAL_WINDOW_SIZE;
    size_t max_response_size = 16 * 1024 * 1024;
    std::string user_agent = "elio-http2/1.0";
    bool enable_push = false;  ///< Advertise SETTINGS_ENABLE_PUSH; pushed responses are not exposed
};

namespace detail {

inline int validate_h2_submit_request_fields(
    method m,
    const url& target,
    std::string_view user_agent,
    std::string_view content_type) noexcept {
    if (m == method::CONNECT) {
        return EOPNOTSUPP;
    }

    if (!target.is_secure()) {
        return EPROTONOSUPPORT;
    }

    if (!is_valid_url_input(target.host_authority()) ||
        !is_valid_request_target(target.path_with_query())) {
        return EINVAL;
    }

    if (!is_valid_header_value(user_agent) ||
        (!content_type.empty() && !is_valid_header_value(content_type))) {
        return EINVAL;
    }

    return 0;
}

} // namespace detail

/// HTTP/2 session (client-side)
class h2_session {
public:
    /// Create client session
    explicit h2_session(tls::tls_stream& stream,
                        h2_session_config config = {})
        : stream_(&stream)
        , config_(std::move(config)) {
        nghttp2_session_callbacks* callbacks;
        nghttp2_session_callbacks_new(&callbacks);

        // Setup callbacks
        // NOTE: send_callback and recv_callback are NOT registered because
        // we use nghttp2_session_mem_send/mem_recv which manage their own
        // I/O buffers directly.
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);
        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
        nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
        nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_callback);
        
        nghttp2_session_client_new(&session_, callbacks, this);
        nghttp2_session_callbacks_del(callbacks);
        
        // Send client connection preface (settings)
        const auto max_streams = static_cast<uint32_t>(
            std::min(config_.max_concurrent_streams,
                     static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
        const auto initial_window = std::min(
            config_.initial_window_size,
            static_cast<uint32_t>(NGHTTP2_MAX_WINDOW_SIZE));
        nghttp2_settings_entry settings[] = {
            {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, max_streams},
            {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, initial_window},
            {NGHTTP2_SETTINGS_ENABLE_PUSH, config_.enable_push ? 1u : 0u}
        };
        nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, settings, 
                               sizeof(settings) / sizeof(settings[0]));
    }
    
    ~h2_session() {
        if (session_) {
            nghttp2_session_del(session_);
        }
    }
    
    // Non-copyable, non-movable.
    // Move is unsafe because nghttp2 callbacks capture `this` and coroutines
    // may hold references to the session's internal maps mid-flight.
    h2_session(const h2_session&) = delete;
    h2_session& operator=(const h2_session&) = delete;
    h2_session(h2_session&&) = delete;
    h2_session& operator=(h2_session&&) = delete;

    /// Get the session configuration.
    const h2_session_config& config() const noexcept { return config_; }
    
    /// Submit a request and get stream ID
    int32_t submit_request(method m, const url& target, std::string_view body = {},
                           std::string_view content_type = {}) {
        if (int validation_error =
                detail::validate_h2_submit_request_fields(
                    m, target, config_.user_agent, content_type);
            validation_error != 0) {
            errno = validation_error;
            return -validation_error;
        }

        // Store header values to ensure they outlive the nghttp2 call
        std::vector<std::string> header_values;
        header_values.reserve(8);
        
        header_values.push_back(std::string(method_to_string(m)));
        auto path = target.path_with_query();
        header_values.push_back(path.empty() ? "/" : path);
        header_values.push_back(target.scheme);
        header_values.push_back(target.host_authority());
        header_values.push_back(config_.user_agent);
        header_values.push_back("*/*");
        
        std::vector<nghttp2_nv> nva;
        nva.push_back(make_nv(":method", header_values[0]));
        nva.push_back(make_nv(":path", header_values[1]));
        nva.push_back(make_nv(":scheme", header_values[2]));
        nva.push_back(make_nv(":authority", header_values[3]));
        nva.push_back(make_nv("user-agent", header_values[4]));
        nva.push_back(make_nv("accept", header_values[5]));
        
        if (!body.empty()) {
            header_values.push_back(std::to_string(body.size()));
            nva.push_back(make_nv("content-length", header_values.back()));
            if (!content_type.empty()) {
                header_values.push_back(std::string(content_type));
                nva.push_back(make_nv("content-type", header_values.back()));
            }
        }
        
        nghttp2_data_provider* data_prd = nullptr;
        nghttp2_data_provider data_provider;
        
        // Store body in pending_bodies_ map so it outlives this function
        int32_t stream_id;
        if (!body.empty()) {
            // Use a temporary stream id placeholder, we'll update after submit
            auto body_ptr = std::make_shared<std::string>(body);
            data_provider.source.ptr = body_ptr.get();
            data_provider.read_callback = [](nghttp2_session*, int32_t, uint8_t* buf, 
                                             size_t length, uint32_t* data_flags,
                                             nghttp2_data_source* source, void*) -> ssize_t {
                auto* body_str = static_cast<std::string*>(source->ptr);
                size_t to_copy = std::min(length, body_str->size());
                std::memcpy(buf, body_str->data(), to_copy);
                body_str->erase(0, to_copy);
                if (body_str->empty()) {
                    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
                }
                return static_cast<ssize_t>(to_copy);
            };
            data_prd = &data_provider;
            
            stream_id = nghttp2_submit_request(session_, nullptr, 
                                               nva.data(), nva.size(),
                                               data_prd, nullptr);
            if (stream_id > 0) {
                pending_bodies_[stream_id] = body_ptr;
            }
        } else {
            stream_id = nghttp2_submit_request(session_, nullptr, 
                                               nva.data(), nva.size(),
                                               nullptr, nullptr);
        }
        
        if (stream_id > 0) {
            streams_[stream_id] = h2_stream{
                .stream_id = stream_id,
                .response_headers = {},
                .response_body = {},
                .response_status = status::ok,
                .headers_complete = false,
                .body_complete = false,
                .closed = false,
                .error = h2_error::none
            };
        }
        
        return stream_id;
    }
    
    /// Process the session (send pending data, receive incoming data, drain)
    coro::task<bool> process() {
        // Phase 1: Send any pending data (outgoing frames queued by submit_request etc.)
        if (!co_await drain_send()) {
            co_return false;
        }

        // Phase 2: Receive data
        if (nghttp2_session_want_read(session_)) {
            uint8_t buf[16384];
            auto result = co_await stream_->read(buf, sizeof(buf));
            if (result.result < 0) {
                ELIO_LOG_ERROR("HTTP/2 read failed");
                co_return false;
            }
            if (result.result == 0) {
                // Connection closed
                co_return false;
            }

            ssize_t rv = nghttp2_session_mem_recv(session_, buf, result.result);
            if (rv < 0) {
                ELIO_LOG_ERROR("HTTP/2 recv error: {}", nghttp2_strerror(static_cast<int>(rv)));
                co_return false;
            }
        }

        // Phase 3: Send frames generated by recv processing (SETTINGS ACK,
        // WINDOW_UPDATE, RST_STREAM, etc.).  Without this drain, protocol-critical
        // frames queued by nghttp2 during recv are never transmitted.
        if (!co_await drain_send()) {
            co_return false;
        }

        co_return true;
    }

private:
    /// Drain all pending outgoing data from nghttp2 to the stream.
    coro::task<bool> drain_send() {
        while (nghttp2_session_want_write(session_)) {
            const uint8_t* data;
            ssize_t len = nghttp2_session_mem_send(session_, &data);
            if (len < 0) {
                ELIO_LOG_ERROR("HTTP/2 send error: {}", nghttp2_strerror(static_cast<int>(len)));
                co_return false;
            }
            if (len > 0) {
                auto result = co_await stream_->write_exactly(data, static_cast<size_t>(len));
                if (result.result != len) {
                    ELIO_LOG_ERROR("HTTP/2 write failed");
                    co_return false;
                }
            }
        }
        co_return true;
    }

public:
    
    /// Wait for a specific stream to complete
    coro::task<std::optional<response>> wait_for_stream(int32_t stream_id) {
        auto it = streams_.find(stream_id);
        if (it == streams_.end()) {
            co_return std::nullopt;
        }
        
        while (!it->second.is_complete() && !it->second.closed) {
            if (!co_await process()) {
                co_return std::nullopt;
            }
            it = streams_.find(stream_id);
            if (it == streams_.end()) {
                co_return std::nullopt;
            }
        }
        
        auto& stream = it->second;
        
        if (stream.error != h2_error::none) {
            if (stream.response_size_exceeded) {
                errno = EMSGSIZE;
            }
            streams_.erase(it);
            co_return std::nullopt;
        }
        
        auto resp = detail::materialize_h2_response(stream);
        
        // Clean up completed stream
        streams_.erase(it);
        
        co_return resp;
    }
    
    /// Check if session is alive
    bool is_alive() const noexcept {
        return nghttp2_session_want_read(session_) || 
               nghttp2_session_want_write(session_);
    }
    
    /// Get stream state
    h2_stream* get_stream(int32_t stream_id) {
        auto it = streams_.find(stream_id);
        return it != streams_.end() ? &it->second : nullptr;
    }
    
    /// Perform GOAWAY and graceful shutdown
    coro::task<void> shutdown() {
        nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE, 
                             nghttp2_session_get_last_proc_stream_id(session_),
                             NGHTTP2_NO_ERROR, nullptr, 0);
        
        // Send GOAWAY frame
        while (nghttp2_session_want_write(session_)) {
            const uint8_t* data;
            ssize_t len = nghttp2_session_mem_send(session_, &data);
            if (len < 0) {
                ELIO_LOG_ERROR("HTTP/2 GOAWAY send error: {}", nghttp2_strerror(static_cast<int>(len)));
                break;
            }
            if (len == 0) {
                break;
            }

            auto result = co_await stream_->write_exactly(data, static_cast<size_t>(len));
            if (result.result != len) {
                ELIO_LOG_ERROR("HTTP/2 GOAWAY write failed");
                break;
            }
        }
    }
    
private:
    static nghttp2_nv make_nv(std::string_view name, std::string_view value) {
        // NOTE: do NOT pass NGHTTP2_NV_FLAG_NO_COPY_NAME/VALUE here. nghttp2 does
        // not serialize HEADERS into its output buffer until a later
        // nghttp2_session_mem_send() call (driven from process()), so the
        // pointed-at storage would have to outlive that call. submit_request()
        // builds nv values in a local std::vector<std::string>, which is
        // destroyed on return, so NO_COPY would dangle and leak arbitrary
        // memory into the HPACK output. Letting nghttp2 copy is the safe
        // default; the cost is one extra allocation per header.
        return {
            const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(name.data())),
            const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(value.data())),
            name.size(),
            value.size(),
            NGHTTP2_NV_FLAG_NONE
        };
    }
    
    // nghttp2 callbacks
    static int on_frame_recv_callback(nghttp2_session*, const nghttp2_frame* frame,
                                       void* user_data) {
        auto* self = static_cast<h2_session*>(user_data);
        
        switch (frame->hd.type) {
            case NGHTTP2_HEADERS:
                if (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) {
                    auto* stream = self->get_stream(frame->hd.stream_id);
                    if (stream) {
                        stream->headers_complete = true;
                        // If END_STREAM is also set, response has no body
                        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                            stream->body_complete = true;
                        }
                    }
                }
                break;
            case NGHTTP2_DATA:
                if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                    auto* stream = self->get_stream(frame->hd.stream_id);
                    if (stream) {
                        stream->body_complete = true;
                    }
                }
                break;
            default:
                break;
        }
        
        return 0;
    }
    
    static int on_data_chunk_recv_callback(nghttp2_session* session, uint8_t,
                                            int32_t stream_id, const uint8_t* data,
                                            size_t len, void* user_data) {
        auto* self = static_cast<h2_session*>(user_data);
        auto* stream = self->get_stream(stream_id);
        if (stream) {
            if (!stream->append_response_data(
                    {reinterpret_cast<const char*>(data), len},
                    self->config_.max_response_size)) {
                if (!stream->response_size_exceeded) {
                    return 0;
                }
                ELIO_LOG_ERROR("HTTP/2 response body on stream {} exceeds "
                               "max_response_size ({} > {})",
                               stream_id, stream->response_body.size() + len,
                               self->config_.max_response_size);
                if (session) {
                    nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE,
                                              stream_id, NGHTTP2_CANCEL);
                }
                return 0;
            }
        }
        // NOTE: nghttp2 automatically manages the flow control window when
        // auto window update is enabled (the default).  No explicit
        // nghttp2_session_consume() call is needed here.
        return 0;
    }
    
    static int on_stream_close_callback(nghttp2_session*, int32_t stream_id,
                                         uint32_t error_code, void* user_data) {
        auto* self = static_cast<h2_session*>(user_data);
        auto* stream = self->get_stream(stream_id);
        if (stream) {
            stream->closed = true;
            stream->error = static_cast<h2_error>(error_code);
            // Only mark body complete on clean close.  On error-close,
            // the body is incomplete and callers should check error first.
            if (error_code == NGHTTP2_NO_ERROR) {
                stream->body_complete = true;
            }
        }
        // Clean up pending body data.
        // NOTE: nghttp2 guarantees no further data provider callbacks after
        // on_stream_close, so erasing pending_bodies_ here is safe.
        self->pending_bodies_.erase(stream_id);
        return 0;
    }
    
    static int on_begin_headers_callback(nghttp2_session*, const nghttp2_frame* frame,
                                          void* user_data) {
        auto* self = static_cast<h2_session*>(user_data);
        if (frame->hd.type == NGHTTP2_HEADERS) {
            if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
                // New response headers
                auto* stream = self->get_stream(frame->hd.stream_id);
                if (stream) {
                    stream->response_headers.clear();
                }
            }
        }
        return 0;
    }
    
    static int on_header_callback(nghttp2_session*, const nghttp2_frame* frame,
                                   const uint8_t* name, size_t namelen,
                                   const uint8_t* value, size_t valuelen,
                                   uint8_t, void* user_data) noexcept {
        auto* self = static_cast<h2_session*>(user_data);

        if (frame->hd.type == NGHTTP2_HEADERS) {
            auto* stream = self->get_stream(frame->hd.stream_id);
            if (stream) {
                std::string_view name_sv(reinterpret_cast<const char*>(name), namelen);
                std::string_view value_sv(reinterpret_cast<const char*>(value), valuelen);

                if (name_sv == ":status") {
                    int status_code = 0;
                    auto [ptr, ec] = std::from_chars(value_sv.data(),
                                                      value_sv.data() + value_sv.size(),
                                                      status_code);
                    if (ec == std::errc{}) {
                        stream->response_status = static_cast<status>(status_code);
                    }
                } else if (!name_sv.starts_with(":")) {
                    // headers::add() validates name/value and throws
                    // std::invalid_argument on RFC 7230 token violations or
                    // CR/LF/NUL in the value. We are inside a C callback
                    // invoked from nghttp2_session_mem_recv() — letting an
                    // exception unwind across the C frames is undefined
                    // behavior and would corrupt nghttp2 internal state. Catch
                    // it and surface the protocol error so nghttp2 resets the
                    // offending stream (RST_STREAM) instead of silently
                    // dropping the header.
                    try {
                        stream->response_headers.add(name_sv, value_sv);
                    } catch (...) {
                        return NGHTTP2_ERR_CALLBACK_FAILURE;
                    }
                }
            }
        }
        return 0;
    }

    nghttp2_session* session_ = nullptr;
    tls::tls_stream* stream_ = nullptr;
    h2_session_config config_;
    std::unordered_map<int32_t, h2_stream> streams_;
    std::unordered_map<int32_t, std::shared_ptr<std::string>> pending_bodies_;
};

} // namespace elio::http
