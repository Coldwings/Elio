#pragma once

#include <elio/http/http_common.hpp>
#include <elio/http/http_message.hpp>
#include <elio/tls/tls_stream.hpp>
#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>

#include <nghttp2/nghttp2.h>

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <queue>
#include <optional>
#include <cstring>

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
    
    bool is_complete() const noexcept {
        return headers_complete && body_complete;
    }
};

/// HTTP/2 session (client-side)
class h2_session {
public:
    /// Create client session
    explicit h2_session(tls::tls_stream& stream)
        : stream_(&stream) {
        nghttp2_session_callbacks* callbacks;
        nghttp2_session_callbacks_new(&callbacks);
        
        // Setup callbacks
        nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
        nghttp2_session_callbacks_set_recv_callback(callbacks, recv_callback);
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);
        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
        nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
        nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_callback);
        
        nghttp2_session_client_new(&session_, callbacks, this);
        nghttp2_session_callbacks_del(callbacks);
        
        // Send client connection preface (settings)
        nghttp2_settings_entry settings[] = {
            {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
            {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 65535},
            {NGHTTP2_SETTINGS_ENABLE_PUSH, 0}  // Disable server push for client
        };
        nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, settings, 
                               sizeof(settings) / sizeof(settings[0]));
    }
    
    ~h2_session() {
        if (session_) {
            nghttp2_session_del(session_);
        }
    }
    
    // Non-copyable, movable
    h2_session(const h2_session&) = delete;
    h2_session& operator=(const h2_session&) = delete;
    h2_session(h2_session&& other) noexcept
        : session_(other.session_)
        , stream_(other.stream_)
        , streams_(std::move(other.streams_))
        , pending_bodies_(std::move(other.pending_bodies_))
        , send_buffer_(std::move(other.send_buffer_))
        , recv_buffer_(std::move(other.recv_buffer_)) {
        other.session_ = nullptr;
        other.stream_ = nullptr;
        // Update user data pointer
        if (session_) {
            nghttp2_session_set_user_data(session_, this);
        }
    }
    
    h2_session& operator=(h2_session&& other) noexcept {
        if (this != &other) {
            if (session_) nghttp2_session_del(session_);
            session_ = other.session_;
            stream_ = other.stream_;
            streams_ = std::move(other.streams_);
            pending_bodies_ = std::move(other.pending_bodies_);
            send_buffer_ = std::move(other.send_buffer_);
            recv_buffer_ = std::move(other.recv_buffer_);
            other.session_ = nullptr;
            other.stream_ = nullptr;
            if (session_) {
                nghttp2_session_set_user_data(session_, this);
            }
        }
        return *this;
    }
    
    /// Submit a request and get stream ID
    int32_t submit_request(method m, const url& target, std::string_view body = {},
                           std::string_view content_type = {}) {
        // Store header values to ensure they outlive the nghttp2 call
        std::vector<std::string> header_values;
        header_values.reserve(8);
        
        header_values.push_back(std::string(method_to_string(m)));
        auto path = target.path_with_query();
        header_values.push_back(path.empty() ? "/" : path);
        header_values.push_back(target.scheme);
        header_values.push_back(target.authority());
        header_values.push_back("elio-http2/1.0");
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
    
    /// Process the session (send pending data, receive incoming data)
    coro::task<bool> process() {
        // Send any pending data
        while (nghttp2_session_want_write(session_)) {
            const uint8_t* data;
            ssize_t len = nghttp2_session_mem_send(session_, &data);
            if (len < 0) {
                ELIO_LOG_ERROR("HTTP/2 send error: {}", nghttp2_strerror(static_cast<int>(len)));
                co_return false;
            }
            if (len > 0) {
                auto result = co_await stream_->write(data, len);
                if (result.result <= 0) {
                    ELIO_LOG_ERROR("HTTP/2 write failed");
                    co_return false;
                }
            }
        }
        
        // Receive data
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
        
        co_return true;
    }
    
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
            co_return std::nullopt;
        }
        
        response resp(stream.response_status);
        for (const auto& [name, value] : stream.response_headers) {
            resp.set_header(name, value);
        }
        resp.set_body(std::move(stream.response_body));
        
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
            if (len > 0) {
                co_await stream_->write(data, len);
            } else {
                break;
            }
        }
    }
    
private:
    static nghttp2_nv make_nv(std::string_view name, std::string_view value) {
        return {
            const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(name.data())),
            const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(value.data())),
            name.size(),
            value.size(),
            NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE
        };
    }
    
    // nghttp2 callbacks
    static ssize_t send_callback(nghttp2_session*, const uint8_t* data,
                                  size_t length, int, void* user_data) {
        auto* self = static_cast<h2_session*>(user_data);
        self->send_buffer_.insert(self->send_buffer_.end(), data, data + length);
        return static_cast<ssize_t>(length);
    }
    
    static ssize_t recv_callback(nghttp2_session*, uint8_t* buf,
                                  size_t length, int, void* user_data) {
        auto* self = static_cast<h2_session*>(user_data);
        if (self->recv_buffer_.empty()) {
            return NGHTTP2_ERR_WOULDBLOCK;
        }
        size_t to_copy = std::min(length, self->recv_buffer_.size());
        std::memcpy(buf, self->recv_buffer_.data(), to_copy);
        self->recv_buffer_.erase(self->recv_buffer_.begin(), 
                                  self->recv_buffer_.begin() + to_copy);
        return static_cast<ssize_t>(to_copy);
    }
    
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
    
    static int on_data_chunk_recv_callback(nghttp2_session*, uint8_t,
                                            int32_t stream_id, const uint8_t* data,
                                            size_t len, void* user_data) {
        auto* self = static_cast<h2_session*>(user_data);
        auto* stream = self->get_stream(stream_id);
        if (stream) {
            stream->response_body.append(reinterpret_cast<const char*>(data), len);
        }
        return 0;
    }
    
    static int on_stream_close_callback(nghttp2_session*, int32_t stream_id,
                                         uint32_t error_code, void* user_data) {
        auto* self = static_cast<h2_session*>(user_data);
        auto* stream = self->get_stream(stream_id);
        if (stream) {
            stream->closed = true;
            stream->body_complete = true;  // Mark as complete even if no data
            stream->error = static_cast<h2_error>(error_code);
        }
        // Clean up pending body data
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
                                   uint8_t, void* user_data) {
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
                    stream->response_headers.set(name_sv, value_sv);
                }
            }
        }
        return 0;
    }
    
    nghttp2_session* session_ = nullptr;
    tls::tls_stream* stream_ = nullptr;
    std::unordered_map<int32_t, h2_stream> streams_;
    std::unordered_map<int32_t, std::shared_ptr<std::string>> pending_bodies_;
    std::vector<uint8_t> send_buffer_;
    std::vector<uint8_t> recv_buffer_;
};

} // namespace elio::http
