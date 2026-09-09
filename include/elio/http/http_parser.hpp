#pragma once

#include <elio/http/http_common.hpp>

#include <climits>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace elio::http {

/// Hard upper bound on a single chunk's declared size (1 GiB). Independent of
/// any per-server max_request_size, this is a safety net to keep
/// request_parser self-contained and to bound the integer arithmetic in
/// parse_chunk_size against pathological inputs.
inline constexpr size_t kMaxChunkSize = static_cast<size_t>(1) << 30;

namespace detail {

inline constexpr bool is_ows(char c) noexcept {
    return c == ' ' || c == '\t';
}

inline constexpr bool is_hex_digit(char c) noexcept {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

inline constexpr size_t hex_digit_value(char c) noexcept {
    if (c >= '0' && c <= '9') return static_cast<size_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<size_t>(c - 'a' + 10);
    return static_cast<size_t>(c - 'A' + 10);
}

inline bool is_quoted_chunk_ext_char(unsigned char c) noexcept {
    return c == '\t' || c == ' ' || (c >= 0x21 && c <= 0x7E) || c >= 0x80;
}

inline bool parse_chunk_ext_quoted_string(std::string_view value, size_t& pos) noexcept {
    if (pos >= value.size() || value[pos] != '"') {
        return false;
    }
    ++pos;

    while (pos < value.size()) {
        unsigned char c = static_cast<unsigned char>(value[pos]);
        if (c == '"') {
            ++pos;
            return true;
        }
        if (c == '\\') {
            ++pos;
            if (pos >= value.size()) {
                return false;
            }
            c = static_cast<unsigned char>(value[pos]);
            if (!(c == '\t' || c == ' ' || (c >= 0x21 && c <= 0x7E) || c >= 0x80)) {
                return false;
            }
            ++pos;
            continue;
        }
        if (!is_quoted_chunk_ext_char(c)) {
            return false;
        }
        ++pos;
    }

    return false;
}

inline bool validate_chunk_extensions(std::string_view value) noexcept {
    size_t pos = 0;

    while (true) {
        while (pos < value.size() && is_ows(value[pos])) {
            ++pos;
        }
        if (pos == value.size()) {
            return true;
        }
        if (value[pos] != ';') {
            return false;
        }
        ++pos;

        while (pos < value.size() && is_ows(value[pos])) {
            ++pos;
        }

        size_t name_start = pos;
        while (pos < value.size() &&
               is_tchar(static_cast<unsigned char>(value[pos]))) {
            ++pos;
        }
        if (pos == name_start) {
            return false;
        }

        while (pos < value.size() && is_ows(value[pos])) {
            ++pos;
        }

        if (pos < value.size() && value[pos] == '=') {
            ++pos;
            while (pos < value.size() && is_ows(value[pos])) {
                ++pos;
            }
            if (pos == value.size()) {
                return false;
            }
            if (value[pos] == '"') {
                if (!parse_chunk_ext_quoted_string(value, pos)) {
                    return false;
                }
            } else {
                size_t token_start = pos;
                while (pos < value.size() &&
                       is_tchar(static_cast<unsigned char>(value[pos]))) {
                    ++pos;
                }
                if (pos == token_start) {
                    return false;
                }
            }
        }
    }
}

inline bool parse_chunk_size_line(std::string_view line,
                                  size_t& chunk_size,
                                  std::string_view& error) noexcept {
    chunk_size = 0;
    size_t pos = 0;

    while (pos < line.size() && is_hex_digit(line[pos])) {
        size_t d = hex_digit_value(line[pos]);
        if (chunk_size > (SIZE_MAX - d) / 16) {
            error = "Chunk size overflow";
            return false;
        }
        chunk_size = chunk_size * 16 + d;
        ++pos;
    }

    if (pos == 0) {
        error = "Empty chunk size";
        return false;
    }
    if (chunk_size > kMaxChunkSize) {
        error = "Chunk size exceeds maximum";
        return false;
    }

    auto rest = trim_ows(line.substr(pos));
    if (!rest.empty() && !validate_chunk_extensions(rest)) {
        error = "Invalid chunk extension";
        return false;
    }

    return true;
}

inline bool validate_chunk_trailer_line(std::string_view line) noexcept {
    auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }

    auto name = line.substr(0, colon);
    auto value = trim_ows(line.substr(colon + 1));
    return is_valid_header_name(name) && is_valid_header_value(value);
}

inline size_t buffered_line_size(std::string_view buffer) noexcept {
    size_t size = buffer.size();
    if (size > 0 && buffer.back() == '\r') {
        --size;
    }
    return size;
}

} // namespace detail

/// HTTP parser state
enum class parse_state {
    start,
    request_line,
    status_line,
    headers,
    body,
    chunk_size,
    chunk_data,
    chunk_trailer,
    complete,
    error
};

/// HTTP parser result
enum class parse_result {
    need_more,      ///< Need more data
    complete,       ///< Parsing complete
    error           ///< Parse error
};

/// HTTP request parser
class request_parser {
public:
    request_parser() = default;

    /// Set maximum number of headers allowed (default: 100)
    void set_max_headers(size_t max) noexcept { max_headers_ = max; }

    /// Set maximum size of a single header line in bytes (default: 8192)
    void set_max_header_size(size_t max) noexcept { max_header_size_ = max; }

    /// Reset parser state
    void reset() {
        state_ = parse_state::request_line;
        method_ = method::GET;
        path_.clear();
        query_.clear();
        version_.clear();
        headers_.clear();
        body_.clear();
        content_length_ = 0;
        body_received_ = 0;
        chunked_ = false;
        chunk_size_ = 0;
        error_message_.clear();
        header_count_ = 0;
    }

    /// Parse incoming data
    /// @param data Data to parse
    /// @return Parse result and number of bytes consumed
    std::pair<parse_result, size_t> parse(std::string_view data) {
        size_t consumed = 0;
        buffer_ += data;

        while (!buffer_.empty() && state_ != parse_state::complete && state_ != parse_state::error) {
            size_t before = buffer_.size();

            switch (state_) {
                case parse_state::request_line:
                    if (!parse_request_line()) {
                        consumed += before - buffer_.size();
                        if (state_ == parse_state::error) {
                            return {parse_result::error, consumed};
                        }
                        return {parse_result::need_more, consumed};
                    }
                    break;

                case parse_state::headers:
                    if (!parse_headers()) {
                        consumed += before - buffer_.size();
                        if (state_ == parse_state::error) {
                            return {parse_result::error, consumed};
                        }
                        return {parse_result::need_more, consumed};
                    }
                    break;

                case parse_state::body:
                    if (!parse_body()) {
                        consumed += before - buffer_.size();
                        return {parse_result::need_more, consumed};
                    }
                    break;

                case parse_state::chunk_size:
                    if (!parse_chunk_size()) {
                        consumed += before - buffer_.size();
                        return {parse_result::need_more, consumed};
                    }
                    break;

                case parse_state::chunk_data:
                    if (!parse_chunk_data()) {
                        consumed += before - buffer_.size();
                        return {parse_result::need_more, consumed};
                    }
                    break;

                case parse_state::chunk_trailer:
                    if (!parse_chunk_trailer()) {
                        consumed += before - buffer_.size();
                        return {parse_result::need_more, consumed};
                    }
                    break;

                default:
                    break;
            }

            consumed += before - buffer_.size();
        }

        // The chunked sub-parsers signal failure by transitioning to
        // parse_state::error and returning true (so the outer loop drops
        // out cleanly). Surface that as parse_result::error to callers —
        // without this check a malformed chunk-size would be reported as
        // "need more data", letting an attacker stall the parser instead
        // of triggering a 400.
        if (state_ == parse_state::error) {
            return {parse_result::error, consumed};
        }
        if (state_ == parse_state::complete) {
            return {parse_result::complete, consumed};
        }

        return {parse_result::need_more, consumed};
    }

    /// Get parsed method
    method get_method() const noexcept { return method_; }
    
    /// Get parsed path
    std::string_view path() const noexcept { return path_; }
    
    /// Get parsed query string
    std::string_view query() const noexcept { return query_; }
    
    /// Get HTTP version
    std::string_view version() const noexcept { return version_; }
    
    /// Get parsed headers
    const headers& get_headers() const noexcept { return headers_; }
    headers& get_headers() noexcept { return headers_; }
    
    /// Get parsed body
    std::string_view body() const noexcept { return body_; }
    
    /// Get error message
    std::string_view error_message() const noexcept { return error_message_; }
    
    /// Check if request is complete
    bool is_complete() const noexcept { return state_ == parse_state::complete; }

    /// Check if there's an error
    bool has_error() const noexcept { return state_ == parse_state::error; }

    /// Move out any unconsumed bytes still sitting in the parser's internal
    /// buffer.  Used after a protocol upgrade (e.g. HTTP -> WebSocket) so that
    /// bytes pipelined behind the upgrade request can be handed to the next
    /// protocol's parser instead of being silently discarded.
    /// After this call the internal buffer is empty.
    std::string take_remaining() {
        std::string out = std::move(buffer_);
        buffer_.clear();
        return out;
    }

    /// Bytes currently held as unconsumed input. Unlike bytes_buffered(), this
    /// does not include body bytes already extracted for the current message.
    size_t buffered_input_size() const noexcept {
        return buffer_.size();
    }

    /// Bytes currently held by the parser (un-consumed buffered input plus
    /// any body bytes already extracted into body_). Callers use this to
    /// enforce aggregate buffered-size limits after reads.
    size_t bytes_buffered() const noexcept {
        return buffer_.size() + body_.size();
    }

    /// Returns the parsed Content-Length once headers are done parsing.
    /// Returns std::nullopt while still consuming the request line/headers
    /// or when the request uses chunked transfer encoding.
    std::optional<size_t> declared_content_length() const noexcept {
        if (chunked_) return std::nullopt;
        switch (state_) {
            case parse_state::body:
            case parse_state::complete:
                return content_length_;
            default:
                return std::nullopt;
        }
    }

private:
    bool parse_request_line() {
        auto line_end = buffer_.find("\r\n");
        if (line_end == std::string::npos) {
            return false;
        }
        
        std::string_view line(buffer_.data(), line_end);
        
        // Parse method
        auto space1 = line.find(' ');
        if (space1 == std::string_view::npos) {
            set_error("Invalid request line: no method");
            return false;
        }
        
        auto method_str = line.substr(0, space1);
        auto m = string_to_method(method_str);
        if (!m) {
            set_error("Unknown HTTP method");
            return false;
        }
        method_ = *m;
        
        // Parse path
        auto path_start = space1 + 1;
        auto space2 = line.find(' ', path_start);
        if (space2 == std::string_view::npos) {
            set_error("Invalid request line: no version");
            return false;
        }
        
        auto uri = line.substr(path_start, space2 - path_start);
        if (!detail::is_valid_request_target(uri)) {
            set_error("Invalid request-target");
            return false;
        }

        // Reject NUL bytes and bare control characters (0x01-0x1F, 0x7F)
        // in the request-target.  These can cause log injection, path
        // traversal in downstream consumers, or protocol confusion when
        // proxying.  Space (0x20) is already excluded by the delimiter
        // search above.
        for (char c : uri) {
            auto uc = static_cast<unsigned char>(c);
            if (uc == 0x00 || (uc >= 0x01 && uc <= 0x1F) || uc == 0x7F) {
                set_error("Invalid character in request-target");
                return false;
            }
        }

        // Split path and query
        auto query_pos = uri.find('?');
        if (query_pos != std::string_view::npos) {
            path_ = uri.substr(0, query_pos);
            query_ = uri.substr(query_pos + 1);
        } else {
            path_ = uri;
        }
        
        // Parse version
        version_ = line.substr(space2 + 1);
        if (version_.empty() || !detail::is_valid_http_version(version_)) {
            set_error("Invalid HTTP version");
            return false;
        }
        
        buffer_.erase(0, line_end + 2);
        state_ = parse_state::headers;
        return true;
    }
    
    bool parse_headers() {
        while (true) {
            auto line_end = buffer_.find("\r\n");
            if (line_end == std::string::npos) {
                if (detail::buffered_line_size(buffer_) > max_header_size_) {
                    set_error("Header line too long");
                }
                return false;
            }

            if (line_end == 0) {
                // Empty line - end of headers
                buffer_.erase(0, 2);

                // RFC 7230 §3.3.3 rule 3: a request that carries both
                // Transfer-Encoding and Content-Length is ambiguous and the
                // canonical request-smuggling vector. Reject as 400 — never
                // pick one over the other.
                bool has_te = headers_.contains("Transfer-Encoding");
                bool has_cl = headers_.contains("Content-Length");
                if (has_te && has_cl) {
                    set_error("Both Transfer-Encoding and Content-Length present (RFC 7230 §3.3.3)");
                    return false;
                }

                if (version_ == "HTTP/1.1") {
                    auto host_values = headers_.get_all("Host");
                    if (host_values.size() != 1) {
                        set_error("HTTP/1.1 requests require exactly one Host header");
                        return false;
                    }
                }

                // Transfer-Encoding takes precedence over (a missing)
                // Content-Length per RFC 7230 §3.3.3. Check chunked FIRST
                // so a malformed/extension-only TE doesn't silently fall
                // through to "no body".
                if (has_te) {
                    if (!headers_.is_chunked()) {
                        // We do not implement other transfer codings; per
                        // RFC 7230 §3.3.1, an unrecognized coding without a
                        // final "chunked" must be rejected.
                        set_error("Unsupported Transfer-Encoding (chunked must be final)");
                        return false;
                    }
                    chunked_ = true;
                    state_ = parse_state::chunk_size;
                } else if (has_cl) {
                    auto len = headers_.content_length();
                    if (!len) {
                        // Header present but unparseable / signed / trailing
                        // garbage — refuse rather than guess.
                        set_error("Invalid Content-Length");
                        return false;
                    }
                    content_length_ = *len;
                    if (content_length_ > 0) {
                        state_ = parse_state::body;
                    } else {
                        state_ = parse_state::complete;
                    }
                } else {
                    // No body
                    state_ = parse_state::complete;
                }
                return true;
            }

            std::string_view line(buffer_.data(), line_end);

            // DoS protection: enforce per-line length limit
            if (line_end > max_header_size_) {
                set_error("Header line too long");
                return false;
            }

            // Parse header
            auto colon = line.find(':');
            if (colon == std::string_view::npos) {
                set_error("Invalid header line");
                return false;
            }

            auto name = line.substr(0, colon);
            auto value = detail::trim_ows(line.substr(colon + 1));

            // Validate name BEFORE calling headers_.add() (which throws on
            // bad input) so we report parser errors uniformly via set_error.
            // RFC 7230 §3.2.4: no whitespace is allowed between field-name
            // and ':' — the validator rejects names with embedded spaces.
            if (!detail::is_valid_header_name(name)) {
                set_error("Invalid header name");
                return false;
            }
            if (!detail::is_valid_header_value(value)) {
                set_error("Invalid header value");
                return false;
            }

            // Detect duplicate Content-Length with conflicting values BEFORE
            // updating the header collection — RFC 7230 §3.3.2.
            if (detail::ascii_iequals(name, "Content-Length")) {
                if (headers_.contains("Content-Length") &&
                    detail::trim_ows(headers_.get("Content-Length")) != value) {
                    set_error("Conflicting Content-Length headers");
                    return false;
                }
            }

            // DoS protection: enforce header count limit. Uses a dedicated
            // counter rather than headers_.size() because the underlying map
            // overwrites duplicate names, so size() only counts unique keys.
            if (header_count_ >= max_headers_) {
                set_error("Too many headers");
                return false;
            }

            ++header_count_;
            headers_.add(name, value);
            buffer_.erase(0, line_end + 2);
        }
    }

    bool parse_body() {
        size_t remaining = content_length_ - body_received_;
        size_t available = std::min(remaining, buffer_.size());

        body_.append(buffer_.data(), available);
        buffer_.erase(0, available);
        body_received_ += available;

        if (body_received_ >= content_length_) {
            state_ = parse_state::complete;
            return true;
        }

        return false;
    }

    bool parse_chunk_size() {
        auto line_end = buffer_.find("\r\n");
        if (line_end == std::string::npos) {
            if (detail::buffered_line_size(buffer_) > max_header_size_) {
                set_error("Chunk size line too long");
                return true;
            }
            return false;
        }
        if (line_end > max_header_size_) {
            set_error("Chunk size line too long");
            return true;
        }

        std::string_view line(buffer_.data(), line_end);

        // Parse hex chunk size with overflow protection. Without the
        // (SIZE_MAX - d) / 16 guard, a malicious peer can send a chunk
        // header like "ffffffffffffffff..." that wraps chunk_size_ to a
        // small value (or zero), letting them either truncate the framed
        // body or trick parse_chunk_data() into appending an attacker-chosen
        // count of bytes. We additionally clamp by kMaxChunkSize so a single
        // chunk cannot OOM the process.
        std::string_view error;
        if (!detail::parse_chunk_size_line(line, chunk_size_, error)) {
            set_error(error);
            return true;
        }

        buffer_.erase(0, line_end + 2);

        if (chunk_size_ == 0) {
            state_ = parse_state::chunk_trailer;
        } else {
            state_ = parse_state::chunk_data;
        }

        return true;
    }

    bool parse_chunk_data() {
        if (buffer_.size() < chunk_size_ + 2) {  // +2 for trailing CRLF
            return false;
        }

        // Validate trailing CRLF per RFC 7230 §4.1
        if (buffer_[chunk_size_] != '\r' || buffer_[chunk_size_ + 1] != '\n') {
            set_error("Missing CRLF after chunk data");
            return true;
        }

        body_.append(buffer_.data(), chunk_size_);
        buffer_.erase(0, chunk_size_ + 2);  // Skip chunk data and CRLF

        state_ = parse_state::chunk_size;
        return true;
    }

    bool parse_chunk_trailer() {
        // Parse trailer headers (usually empty)
        auto line_end = buffer_.find("\r\n");
        if (line_end == std::string::npos) {
            if (detail::buffered_line_size(buffer_) > max_header_size_) {
                set_error("Trailer line too long");
                return true;
            }
            return false;
        }

        if (line_end == 0) {
            // Empty line - end of chunked body
            buffer_.erase(0, 2);
            state_ = parse_state::complete;
            return true;
        }

        if (line_end > max_header_size_) {
            set_error("Trailer line too long");
            return true;
        }
        if (header_count_ >= max_headers_) {
            set_error("Too many headers");
            return true;
        }

        std::string_view line(buffer_.data(), line_end);
        if (!detail::validate_chunk_trailer_line(line)) {
            set_error("Invalid trailer header");
            return true;
        }

        ++header_count_;
        buffer_.erase(0, line_end + 2);
        return true;
    }

    void set_error(std::string_view msg) {
        state_ = parse_state::error;
        error_message_ = msg;
    }

    parse_state state_ = parse_state::request_line;
    method method_ = method::GET;
    std::string path_;
    std::string query_;
    std::string version_;
    headers headers_;
    std::string body_;
    std::string buffer_;
    size_t content_length_ = 0;
    size_t body_received_ = 0;
    bool chunked_ = false;
    size_t chunk_size_ = 0;
    std::string error_message_;

    // DoS protection limits
    size_t max_headers_ = 100;
    size_t max_header_size_ = 8192;
    size_t header_count_ = 0;
};

/// One increment of response framing progress.
enum class response_event {
    need_more,
    headers_complete,
    body,
    message_complete,
    protocol_handoff,
    error
};

struct response_decode_result {
    response_event event = response_event::need_more;
    /// Bytes accepted from this call's input, never from an earlier call.
    size_t consumed = 0;
    /// Borrowed from this call's input; invalidated when that input changes.
    std::string_view body;
};

/// Incremental HTTP/1 response framing decoder. Payload is never buffered:
/// each body event borrows a slice of the supplied input. Only framing lines
/// and parsed headers are owned. Drain events (including with empty input)
/// until need_more before reading again; retain any unconsumed input.
class response_decoder {
public:
    void set_max_headers(size_t max) noexcept { max_headers_ = max; }
    void set_max_header_size(size_t max) noexcept { max_header_size_ = max; }
    void set_request_method(method value) noexcept { request_method_ = value; }

    void reset() {
        state_ = state::status_line;
        status_ = status::ok;
        version_.clear();
        reason_.clear();
        headers_.clear();
        line_.clear();
        error_message_.clear();
        remaining_ = 0;
        header_count_ = 0;
        headers_complete_ = false;
        close_delimited_ = false;
        handoff_ = false;
        limit_exceeded_ = false;
        request_method_.reset();
    }

    response_decode_result decode(std::string_view input) {
        size_t used = 0;
        for (;;) {
            if (state_ == state::error) return {response_event::error, used, {}};
            if (state_ == state::complete) {
                return {handoff_ ? response_event::protocol_handoff
                                 : response_event::message_complete, used, {}};
            }
            if (state_ == state::fixed_body || state_ == state::close_body ||
                state_ == state::chunk_body) {
                if (input.empty()) return {response_event::need_more, used, {}};
                const size_t count = state_ == state::close_body
                    ? input.size() : std::min(remaining_, input.size());
                const auto payload = input.substr(0, count);
                if (state_ != state::close_body) {
                    remaining_ -= count;
                    if (remaining_ == 0) {
                        state_ = state_ == state::fixed_body
                            ? state::complete : state::chunk_cr;
                    }
                }
                return {response_event::body, used + count, payload};
            }
            if (state_ == state::chunk_cr || state_ == state::chunk_lf) {
                if (input.empty()) return {response_event::need_more, used, {}};
                const bool cr = state_ == state::chunk_cr;
                if (input.front() != (cr ? '\r' : '\n')) {
                    fail("Missing CRLF after chunk data");
                    continue;
                }
                ++used;
                input.remove_prefix(1);
                state_ = cr ? state::chunk_lf : state::chunk_size;
                continue;
            }

            // Only protocol metadata is copied. A split CRLF requires at
            // most max_header_size + 2 bytes, independent of payload size.
            if (input.empty()) return {response_event::need_more, used, {}};
            const char ch = input.front();
            input.remove_prefix(1);
            ++used;
            if (!line_.empty() && line_.back() == '\r' && ch != '\n') {
                fail("Invalid framing line ending");
                continue;
            }
            if (ch == '\n') {
                if (line_.empty() || line_.back() != '\r') {
                    fail("Invalid framing line ending");
                    continue;
                }
                line_.pop_back();
                const bool header_boundary =
                    state_ == state::headers && line_.empty();
                process_line(line_);
                line_.clear();
                if (has_error()) continue;
                if (header_boundary) {
                    headers_complete_ = true;
                    return {response_event::headers_complete, used, {}};
                }
                continue;
            }
            if (ch != '\r' && line_.size() >= max_header_size_) {
                fail_limit(state_ == state::status_line ? "Status line too long" :
                     state_ == state::chunk_size ? "Chunk size line too long" :
                     state_ == state::trailers ? "Trailer line too long" :
                     "Header line too long");
                continue;
            }
            line_.push_back(ch);
        }
    }

    response_decode_result finish_eof() {
        if (state_ == state::close_body) state_ = state::complete;
        if (!is_complete() && !has_error()) {
            fail("Connection closed before response complete");
        }
        return decode({});
    }

    status get_status() const noexcept { return status_; }
    uint16_t status_code() const noexcept { return static_cast<uint16_t>(status_); }
    std::string_view version() const noexcept { return version_; }
    std::string_view reason() const noexcept { return reason_; }
    const headers& get_headers() const noexcept { return headers_; }
    headers& get_headers() noexcept { return headers_; }
    std::string_view error_message() const noexcept { return error_message_; }
    bool headers_complete() const noexcept { return headers_complete_; }
    bool is_complete() const noexcept { return state_ == state::complete; }
    bool has_error() const noexcept { return state_ == state::error; }
    bool limit_exceeded() const noexcept { return limit_exceeded_; }
    bool is_close_delimited() const noexcept { return close_delimited_; }
    /// Pending framing bytes only; parsed header storage is separately
    /// bounded by max_headers and max_header_size. No body storage exists.
    size_t bytes_buffered() const noexcept { return line_.size(); }

private:
    friend class response_parser;
    enum class state {
        status_line, headers, fixed_body, close_body, chunk_size, chunk_body,
        chunk_cr, chunk_lf, trailers, complete, error
    };

    void fail(std::string_view message) {
        error_message_ = message;
        state_ = state::error;
    }

    void fail_limit(std::string_view message) {
        limit_exceeded_ = true;
        fail(message);
    }

    void process_line(std::string_view line) {
        if (state_ == state::status_line) {
            const auto first = line.find(' ');
            if (first == std::string_view::npos) {
                fail("Invalid status line: no version");
                return;
            }
            if (!detail::is_valid_http_version(line.substr(0, first))) {
                fail("Invalid HTTP version");
                return;
            }
            version_ = line.substr(0, first);
            auto rest = line.substr(first + 1);
            const auto space = rest.find(' ');
            const auto code = rest.substr(0, space);
            uint16_t number = 0;
            const auto [end, ec] =
                std::from_chars(code.data(), code.data() + code.size(), number);
            if (code.size() != 3 || ec != std::errc{} ||
                end != code.data() + code.size()) {
                fail("Invalid status code");
                return;
            }
            status_ = static_cast<status>(number);
            if (space != std::string_view::npos) reason_ = rest.substr(space + 1);
            state_ = state::headers;
            return;
        }
        if (state_ == state::chunk_size) {
            std::string_view error;
            if (!detail::parse_chunk_size_line(line, remaining_, error)) {
                fail(error);
                return;
            }
            state_ = remaining_ == 0 ? state::trailers : state::chunk_body;
            return;
        }
        if (state_ == state::trailers) {
            if (line.empty()) {
                state_ = state::complete;
            } else if (header_count_ >= max_headers_) {
                fail_limit("Too many headers");
            } else if (!detail::validate_chunk_trailer_line(line)) {
                fail("Invalid trailer header");
            } else {
                ++header_count_;
            }
            return;
        }
        if (line.empty()) {
            select_framing();
            return;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            fail("Invalid header line");
            return;
        }
        const auto name = line.substr(0, colon);
        const auto value = detail::trim_ows(line.substr(colon + 1));
        if (!detail::is_valid_header_name(name)) {
            fail("Invalid header name");
            return;
        }
        if (!detail::is_valid_header_value(value)) {
            fail("Invalid header value");
            return;
        }
        if (detail::ascii_iequals(name, "Content-Length") &&
            headers_.contains("Content-Length") &&
            detail::trim_ows(headers_.get("Content-Length")) != value) {
            fail("Conflicting Content-Length headers");
            return;
        }
        if (header_count_ >= max_headers_) {
            fail_limit("Too many headers");
            return;
        }
        ++header_count_;
        headers_.add(name, value);
    }

    void select_framing() {
        const auto code = status_code();
        handoff_ = code == 101 ||
            (request_method_ == method::CONNECT && code >= 200 && code < 300);
        if (handoff_ || request_method_ == method::HEAD ||
            detail::status_forbids_response_body(status_)) {
            state_ = state::complete;
            return;
        }
        const bool has_te = headers_.contains("Transfer-Encoding");
        const bool has_cl = headers_.contains("Content-Length");
        if (has_te && has_cl) {
            fail("Both Transfer-Encoding and Content-Length present");
        } else if (has_te) {
            // No other transfer coding is implemented. Merely accepting
            // a final chunked token would expose still-encoded payload.
            if (!detail::ascii_iequals(
                    detail::trim_ows(headers_.get("Transfer-Encoding")), "chunked")) {
                fail("Unsupported Transfer-Encoding");
                return;
            }
            state_ = state::chunk_size;
        } else if (has_cl) {
            const auto length = headers_.content_length();
            if (!length) {
                fail("Invalid Content-Length");
                return;
            }
            remaining_ = *length;
            state_ = remaining_ == 0 ? state::complete : state::fixed_body;
        } else {
            close_delimited_ = true;
            state_ = state::close_body;
        }
    }

    state state_ = state::status_line;
    status status_ = status::ok;
    std::string version_;
    std::string reason_;
    headers headers_;
    std::string line_;
    std::string error_message_;
    std::optional<method> request_method_;
    size_t remaining_ = 0;
    size_t max_headers_ = 100;
    size_t max_header_size_ = 8192;
    size_t header_count_ = 0;
    bool headers_complete_ = false;
    bool close_delimited_ = false;
    bool handoff_ = false;
    bool limit_exceeded_ = false;
};

/// Accumulating compatibility adapter. For borrowed incremental payload use
/// response_decoder instead. Unlike decoder::consumed, parse().second counts
/// bytes retired from old AND new buffered input. reset() preserves remaining
/// input, and callers must not re-feed that input without take_remaining().
/// Chunk payload is accumulated incrementally, before its trailing CRLF is
/// validated. A partial body is not proof of a complete, valid response.
class response_parser {
public:
    void set_max_headers(size_t max) noexcept { decoder_.set_max_headers(max); }
    void set_max_header_size(size_t max) noexcept { decoder_.set_max_header_size(max); }
    void set_request_method(method value) noexcept { decoder_.set_request_method(value); }

    void reset() {
        buffer_.insert(0, decoder_.line_);
        decoder_.reset();
        body_.clear();
    }

    std::pair<parse_result, size_t> parse(std::string_view data) {
        const size_t old_pending = decoder_.bytes_buffered();
        buffer_.append(data);
        size_t offset = 0;
        for (;;) {
            auto result = decoder_.decode(std::string_view(buffer_).substr(offset));
            offset += result.consumed;
            if (result.event == response_event::body) body_.append(result.body);
            if (result.event == response_event::need_more ||
                result.event == response_event::error ||
                result.event == response_event::message_complete ||
                result.event == response_event::protocol_handoff) break;
        }
        buffer_.erase(0, offset);
        // Incomplete framing lines remain unretired until the next feed.
        const size_t consumed = old_pending + offset - decoder_.bytes_buffered();
        return {result_kind(), consumed};
    }

    std::pair<parse_result, size_t> finish_eof() {
        decoder_.finish_eof();
        return {result_kind(), 0};
    }

    status get_status() const noexcept { return decoder_.get_status(); }
    uint16_t status_code() const noexcept { return decoder_.status_code(); }
    std::string_view version() const noexcept { return decoder_.version(); }
    std::string_view reason() const noexcept { return decoder_.reason(); }
    const headers& get_headers() const noexcept { return decoder_.get_headers(); }
    headers& get_headers() noexcept { return decoder_.get_headers(); }
    std::string_view body() const noexcept { return body_; }
    std::string take_body() { return std::move(body_); }
    std::string_view error_message() const noexcept { return decoder_.error_message(); }
    bool headers_complete() const noexcept { return decoder_.headers_complete(); }
    bool is_complete() const noexcept { return decoder_.is_complete(); }
    bool has_error() const noexcept { return decoder_.has_error(); }
    bool is_close_delimited() const noexcept { return decoder_.is_close_delimited(); }
    std::string take_remaining() {
        std::string out = std::move(decoder_.line_);
        decoder_.line_.clear();
        out += buffer_;
        buffer_.clear();
        return out;
    }
    size_t bytes_buffered() const noexcept {
        return body_.size() + bytes_remaining();
    }
    size_t bytes_remaining() const noexcept {
        return decoder_.bytes_buffered() + buffer_.size();
    }

private:
    parse_result result_kind() const noexcept {
        return has_error() ? parse_result::error :
               is_complete() ? parse_result::complete : parse_result::need_more;
    }
    response_decoder decoder_;
    std::string body_;
    std::string buffer_;
};

} // namespace elio::http
