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

/// HTTP response parser
class response_parser {
public:
    response_parser() = default;

    /// Set maximum number of headers allowed (default: 100)
    void set_max_headers(size_t max) noexcept { max_headers_ = max; }

    /// Set maximum size of a single header line in bytes (default: 8192)
    void set_max_header_size(size_t max) noexcept { max_header_size_ = max; }

    /// Set the request method whose response is being parsed. This lets the
    /// parser apply response-body rules for HEAD responses.
    void set_request_method(method m) noexcept { request_method_ = m; }

    /// Reset parser state
    void reset() {
        state_ = parse_state::status_line;
        status_ = status::ok;
        version_.clear();
        reason_.clear();
        headers_.clear();
        body_.clear();
        content_length_ = 0;
        body_received_ = 0;
        chunked_ = false;
        close_delimited_ = false;
        chunk_size_ = 0;
        error_message_.clear();
        header_count_ = 0;
        request_method_.reset();
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
                case parse_state::status_line:
                    if (!parse_status_line()) {
                        if (state_ == parse_state::error) {
                            return {parse_result::error, consumed};
                        }
                        return {parse_result::need_more, consumed};
                    }
                    break;
                    
                case parse_state::headers:
                    if (!parse_headers()) {
                        if (state_ == parse_state::error) {
                            return {parse_result::error, consumed};
                        }
                        return {parse_result::need_more, consumed};
                    }
                    break;
                    
                case parse_state::body:
                    if (!parse_body()) {
                        return {parse_result::need_more, consumed};
                    }
                    break;
                    
                case parse_state::chunk_size:
                    if (!parse_chunk_size()) {
                        return {parse_result::need_more, consumed};
                    }
                    break;
                    
                case parse_state::chunk_data:
                    if (!parse_chunk_data()) {
                        return {parse_result::need_more, consumed};
                    }
                    break;
                    
                case parse_state::chunk_trailer:
                    if (!parse_chunk_trailer()) {
                        return {parse_result::need_more, consumed};
                    }
                    break;
                    
                default:
                    break;
            }
            
            consumed += before - buffer_.size();
        }

        // Mirror request_parser: surface error-state from the chunked
        // sub-parsers as parse_result::error.
        if (state_ == parse_state::error) {
            return {parse_result::error, consumed};
        }
        if (state_ == parse_state::complete) {
            return {parse_result::complete, consumed};
        }

        return {parse_result::need_more, consumed};
    }

    /// Finish parsing when the peer closes the connection.
    ///
    /// Close-delimited HTTP/1 responses do not become complete until EOF. For
    /// fixed-length or chunked responses, EOF before completion is a parse
    /// error because the advertised framing was truncated.
    std::pair<parse_result, size_t> finish_eof() {
        if (state_ == parse_state::error) {
            return {parse_result::error, 0};
        }
        if (state_ == parse_state::complete) {
            return {parse_result::complete, 0};
        }
        if (close_delimited_ && state_ == parse_state::body) {
            state_ = parse_state::complete;
            return {parse_result::complete, 0};
        }
        set_error("Connection closed before response complete");
        return {parse_result::error, 0};
    }

    /// Get parsed status
    status get_status() const noexcept { return status_; }
    
    /// Get status code as integer
    uint16_t status_code() const noexcept { return static_cast<uint16_t>(status_); }
    
    /// Get HTTP version
    std::string_view version() const noexcept { return version_; }
    
    /// Get reason phrase
    std::string_view reason() const noexcept { return reason_; }
    
    /// Get parsed headers
    const headers& get_headers() const noexcept { return headers_; }
    headers& get_headers() noexcept { return headers_; }
    
    /// Get parsed body
    std::string_view body() const noexcept { return body_; }
    
    /// Take ownership of body
    std::string take_body() { return std::move(body_); }
    
    /// Get error message
    std::string_view error_message() const noexcept { return error_message_; }
    
    /// Check if response is complete
    bool is_complete() const noexcept { return state_ == parse_state::complete; }

    /// Check if there's an error
    bool has_error() const noexcept { return state_ == parse_state::error; }

    /// True when this response body is delimited only by connection close.
    bool is_close_delimited() const noexcept { return close_delimited_; }

    /// Move out any unconsumed bytes still sitting in the parser's internal
    /// buffer.  Used after a protocol upgrade (e.g. WebSocket client receiving
    /// the 101 response with a piggybacked frame in the same TCP segment).
    /// After this call the internal buffer is empty.
    std::string take_remaining() {
        std::string out = std::move(buffer_);
        buffer_.clear();
        return out;
    }

    /// Bytes currently held by the parser (un-consumed buffered input plus
    /// any body bytes already extracted into body_). The HTTP client uses this
    /// to enforce client_config::max_response_size after every read so a
    /// hostile server cannot OOM the client by streaming a huge body.
    size_t bytes_buffered() const noexcept {
        return buffer_.size() + body_.size();
    }

    /// Bytes still sitting in the parser's input buffer that were NOT consumed
    /// by the parsed message. After is_complete() is true, a non-zero value
    /// means the server pipelined extra bytes after the response (e.g. a
    /// response-splitting payload). The HTTP client uses this to refuse to
    /// return such a connection to the keep-alive pool: those bytes would
    /// otherwise be misread as the head of the next response.
    size_t bytes_remaining() const noexcept {
        return buffer_.size();
    }

private:
    bool response_body_forbidden() const noexcept {
        if (request_method_ && *request_method_ == method::HEAD) {
            return true;
        }

        return detail::status_forbids_response_body(status_);
    }

    bool parse_status_line() {
        auto line_end = buffer_.find("\r\n");
        if (line_end == std::string::npos) {
            return false;
        }
        
        std::string_view line(buffer_.data(), line_end);
        
        // Parse version
        auto space1 = line.find(' ');
        if (space1 == std::string_view::npos) {
            set_error("Invalid status line: no version");
            return false;
        }
        
        version_ = line.substr(0, space1);
        if (version_.empty() || !detail::is_valid_http_version(version_)) {
            set_error("Invalid HTTP version");
            return false;
        }
        
        // Parse status code
        auto status_start = space1 + 1;
        auto space2 = line.find(' ', status_start);
        std::string_view status_str;
        if (space2 == std::string_view::npos) {
            status_str = line.substr(status_start);
        } else {
            status_str = line.substr(status_start, space2 - status_start);
            reason_ = line.substr(space2 + 1);
        }
        
        uint16_t code = 0;
        auto status_begin = status_str.data();
        auto status_end = status_begin + status_str.size();
        auto [ptr, ec] = std::from_chars(status_begin, status_end, code);
        if (status_str.size() != 3 || ec != std::errc{} || ptr != status_end) {
            set_error("Invalid status code");
            return false;
        }
        status_ = static_cast<status>(code);
        
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

                // Same RFC 7230 §3.3.3 rule applies to responses: a server
                // returning both Transfer-Encoding and Content-Length is
                // suspect — silently picking one enables response smuggling
                // through downstream proxies.
                bool has_te = headers_.contains("Transfer-Encoding");
                bool has_cl = headers_.contains("Content-Length");

                if (response_body_forbidden()) {
                    state_ = parse_state::complete;
                    return true;
                }

                if (has_te && has_cl) {
                    set_error("Both Transfer-Encoding and Content-Length present");
                    return false;
                }

                if (has_te) {
                    if (!headers_.is_chunked()) {
                        set_error("Unsupported Transfer-Encoding (chunked must be final)");
                        return false;
                    }
                    chunked_ = true;
                    state_ = parse_state::chunk_size;
                } else if (has_cl) {
                    auto len = headers_.content_length();
                    if (!len) {
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
                    // Response body length is determined by connection close.
                    close_delimited_ = true;
                    state_ = parse_state::body;
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

            if (!detail::is_valid_header_name(name)) {
                set_error("Invalid header name");
                return false;
            }
            if (!detail::is_valid_header_value(value)) {
                set_error("Invalid header value");
                return false;
            }
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
        if (close_delimited_) {
            body_received_ += buffer_.size();
            body_.append(buffer_.data(), buffer_.size());
            buffer_.clear();
            return true;
        }

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

        // See request_parser::parse_chunk_size for the full rationale on
        // the overflow guard and kMaxChunkSize clamp.
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
        if (buffer_.size() < chunk_size_ + 2) {
            return false;
        }

        // Validate trailing CRLF per RFC 7230 §4.1
        if (buffer_[chunk_size_] != '\r' || buffer_[chunk_size_ + 1] != '\n') {
            set_error("Missing CRLF after chunk data");
            return true;
        }

        body_.append(buffer_.data(), chunk_size_);
        buffer_.erase(0, chunk_size_ + 2);

        state_ = parse_state::chunk_size;
        return true;
    }
    
    bool parse_chunk_trailer() {
        auto line_end = buffer_.find("\r\n");
        if (line_end == std::string::npos) {
            if (detail::buffered_line_size(buffer_) > max_header_size_) {
                set_error("Trailer line too long");
                return true;
            }
            return false;
        }
        
        if (line_end == 0) {
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
    
    parse_state state_ = parse_state::status_line;
    status status_ = status::ok;
    std::string version_;
    std::string reason_;
    headers headers_;
    std::string body_;
    std::string buffer_;
    size_t content_length_ = 0;
    size_t body_received_ = 0;
    bool chunked_ = false;
    bool close_delimited_ = false;
    size_t chunk_size_ = 0;
    std::string error_message_;
    std::optional<method> request_method_;

    // DoS protection limits
    size_t max_headers_ = 100;
    size_t max_header_size_ = 8192;
    size_t header_count_ = 0;
};

} // namespace elio::http
