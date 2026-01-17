#pragma once

#include <elio/http/http_common.hpp>

#include <string>
#include <string_view>
#include <optional>
#include <cstring>

namespace elio::http {

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
        if (!version_.starts_with("HTTP/")) {
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
                return false;
            }
            
            if (line_end == 0) {
                // Empty line - end of headers
                buffer_.erase(0, 2);
                
                // Check for body
                if (auto len = headers_.content_length()) {
                    content_length_ = *len;
                    if (content_length_ > 0) {
                        state_ = parse_state::body;
                    } else {
                        state_ = parse_state::complete;
                    }
                } else if (headers_.is_chunked()) {
                    chunked_ = true;
                    state_ = parse_state::chunk_size;
                } else {
                    // No body
                    state_ = parse_state::complete;
                }
                return true;
            }
            
            std::string_view line(buffer_.data(), line_end);
            
            // Parse header
            auto colon = line.find(':');
            if (colon == std::string_view::npos) {
                set_error("Invalid header line");
                return false;
            }
            
            auto name = line.substr(0, colon);
            auto value = line.substr(colon + 1);
            
            // Trim leading whitespace from value
            while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) {
                value = value.substr(1);
            }
            
            // Trim trailing whitespace from value
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value = value.substr(0, value.size() - 1);
            }
            
            headers_.set(name, value);
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
            return false;
        }
        
        std::string_view line(buffer_.data(), line_end);
        
        // Parse hex chunk size
        chunk_size_ = 0;
        for (char c : line) {
            if (c >= '0' && c <= '9') {
                chunk_size_ = chunk_size_ * 16 + (c - '0');
            } else if (c >= 'a' && c <= 'f') {
                chunk_size_ = chunk_size_ * 16 + (c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                chunk_size_ = chunk_size_ * 16 + (c - 'A' + 10);
            } else if (c == ';') {
                // Chunk extension - ignore
                break;
            } else {
                break;
            }
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
        
        body_.append(buffer_.data(), chunk_size_);
        buffer_.erase(0, chunk_size_ + 2);  // Skip chunk data and CRLF
        
        state_ = parse_state::chunk_size;
        return true;
    }
    
    bool parse_chunk_trailer() {
        // Parse trailer headers (usually empty)
        auto line_end = buffer_.find("\r\n");
        if (line_end == std::string::npos) {
            return false;
        }
        
        if (line_end == 0) {
            // Empty line - end of chunked body
            buffer_.erase(0, 2);
            state_ = parse_state::complete;
            return true;
        }
        
        // Skip trailer header
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
};

/// HTTP response parser
class response_parser {
public:
    response_parser() = default;
    
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
        chunk_size_ = 0;
        error_message_.clear();
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
        
        if (state_ == parse_state::complete) {
            return {parse_result::complete, consumed};
        }
        
        return {parse_result::need_more, consumed};
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
    
private:
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
        if (!version_.starts_with("HTTP/")) {
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
        auto [ptr, ec] = std::from_chars(status_str.data(), status_str.data() + status_str.size(), code);
        if (ec != std::errc{}) {
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
                return false;
            }
            
            if (line_end == 0) {
                // Empty line - end of headers
                buffer_.erase(0, 2);
                
                // Check for body
                if (auto len = headers_.content_length()) {
                    content_length_ = *len;
                    if (content_length_ > 0) {
                        state_ = parse_state::body;
                    } else {
                        state_ = parse_state::complete;
                    }
                } else if (headers_.is_chunked()) {
                    chunked_ = true;
                    state_ = parse_state::chunk_size;
                } else {
                    // No body or connection close
                    state_ = parse_state::complete;
                }
                return true;
            }
            
            std::string_view line(buffer_.data(), line_end);
            
            // Parse header
            auto colon = line.find(':');
            if (colon == std::string_view::npos) {
                set_error("Invalid header line");
                return false;
            }
            
            auto name = line.substr(0, colon);
            auto value = line.substr(colon + 1);
            
            // Trim whitespace
            while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) {
                value = value.substr(1);
            }
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value = value.substr(0, value.size() - 1);
            }
            
            headers_.set(name, value);
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
            return false;
        }
        
        std::string_view line(buffer_.data(), line_end);
        
        chunk_size_ = 0;
        for (char c : line) {
            if (c >= '0' && c <= '9') {
                chunk_size_ = chunk_size_ * 16 + (c - '0');
            } else if (c >= 'a' && c <= 'f') {
                chunk_size_ = chunk_size_ * 16 + (c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                chunk_size_ = chunk_size_ * 16 + (c - 'A' + 10);
            } else {
                break;
            }
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
        
        body_.append(buffer_.data(), chunk_size_);
        buffer_.erase(0, chunk_size_ + 2);
        
        state_ = parse_state::chunk_size;
        return true;
    }
    
    bool parse_chunk_trailer() {
        auto line_end = buffer_.find("\r\n");
        if (line_end == std::string::npos) {
            return false;
        }
        
        if (line_end == 0) {
            buffer_.erase(0, 2);
            state_ = parse_state::complete;
            return true;
        }
        
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
    size_t chunk_size_ = 0;
    std::string error_message_;
};

} // namespace elio::http
