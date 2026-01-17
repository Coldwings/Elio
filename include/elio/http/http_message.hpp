#pragma once

#include <elio/http/http_common.hpp>
#include <elio/http/http_parser.hpp>

#include <string>
#include <string_view>

namespace elio::http {

/// HTTP request
class request {
public:
    request() = default;
    
    /// Create a request with method and path
    request(method m, std::string_view path)
        : method_(m), path_(path) {}
    
    /// Get/set method
    method get_method() const noexcept { return method_; }
    void set_method(method m) noexcept { method_ = m; }
    
    /// Get/set path
    std::string_view path() const noexcept { return path_; }
    void set_path(std::string_view p) { path_ = p; }
    
    /// Get/set query string
    std::string_view query() const noexcept { return query_; }
    void set_query(std::string_view q) { query_ = q; }
    
    /// Get/set HTTP version
    std::string_view version() const noexcept { return version_; }
    void set_version(std::string_view v) { version_ = v; }
    
    /// Get/set headers
    const headers& get_headers() const noexcept { return headers_; }
    headers& get_headers() noexcept { return headers_; }
    
    /// Set a header
    void set_header(std::string_view name, std::string_view value) {
        headers_.set(name, value);
    }
    
    /// Get a header
    std::string_view header(std::string_view name) const {
        return headers_.get(name);
    }
    
    /// Get/set body
    std::string_view body() const noexcept { return body_; }
    void set_body(std::string_view b) { body_ = b; }
    void set_body(std::string&& b) { body_ = std::move(b); }
    
    /// Get host header
    std::string_view host() const { return headers_.get("Host"); }
    
    /// Set host header
    void set_host(std::string_view h) { headers_.set("Host", h); }
    
    /// Get content type
    std::string_view content_type() const { return headers_.content_type(); }
    
    /// Set content type
    void set_content_type(std::string_view type) { headers_.set_content_type(type); }
    
    /// Get path with query
    std::string path_with_query() const {
        if (query_.empty()) return path_.empty() ? "/" : path_;
        return (path_.empty() ? "/" : path_) + "?" + query_;
    }
    
    /// Parse query string parameters
    std::unordered_map<std::string, std::string> query_params() const {
        return parse_query_string(query_);
    }
    
    /// Serialize request to string (HTTP/1.1 format)
    std::string serialize() const {
        std::string result;
        
        // Request line
        result += method_to_string(method_);
        result += ' ';
        result += path_with_query();
        result += ' ';
        result += version_.empty() ? "HTTP/1.1" : version_;
        result += "\r\n";
        
        // Headers
        result += headers_.serialize();
        
        // End of headers
        result += "\r\n";
        
        // Body
        if (!body_.empty()) {
            result += body_;
        }
        
        return result;
    }
    
    /// Create from parser
    static request from_parser(request_parser& parser) {
        request req;
        req.method_ = parser.get_method();
        req.path_ = parser.path();
        req.query_ = parser.query();
        req.version_ = parser.version();
        req.headers_ = parser.get_headers();
        req.body_ = parser.body();
        return req;
    }
    
private:
    method method_ = method::GET;
    std::string path_ = "/";
    std::string query_;
    std::string version_ = "HTTP/1.1";
    headers headers_;
    std::string body_;
};

/// HTTP response
class response {
public:
    response() = default;
    
    /// Create a response with status
    explicit response(status s) : status_(s) {}
    
    /// Create a response with status and body
    response(status s, std::string_view body, std::string_view content_type = mime::text_plain)
        : status_(s), body_(body) {
        headers_.set_content_type(content_type);
        headers_.set_content_length(body_.size());
    }
    
    /// Get/set status
    status get_status() const noexcept { return status_; }
    void set_status(status s) noexcept { status_ = s; }
    
    /// Get status code as integer
    uint16_t status_code() const noexcept { return static_cast<uint16_t>(status_); }
    
    /// Get/set HTTP version
    std::string_view version() const noexcept { return version_; }
    void set_version(std::string_view v) { version_ = v; }
    
    /// Get/set headers
    const headers& get_headers() const noexcept { return headers_; }
    headers& get_headers() noexcept { return headers_; }
    
    /// Set a header
    void set_header(std::string_view name, std::string_view value) {
        headers_.set(name, value);
    }
    
    /// Get a header
    std::string_view header(std::string_view name) const {
        return headers_.get(name);
    }
    
    /// Get/set body
    std::string_view body() const noexcept { return body_; }
    void set_body(std::string_view b) { 
        body_ = b; 
        headers_.set_content_length(body_.size());
    }
    void set_body(std::string&& b) { 
        body_ = std::move(b);
        headers_.set_content_length(body_.size());
    }
    
    /// Get content type
    std::string_view content_type() const { return headers_.content_type(); }
    
    /// Set content type
    void set_content_type(std::string_view type) { headers_.set_content_type(type); }
    
    /// Check if response indicates success (2xx)
    bool is_success() const noexcept {
        auto code = static_cast<uint16_t>(status_);
        return code >= 200 && code < 300;
    }
    
    /// Check if response is a redirect (3xx)
    bool is_redirect() const noexcept {
        auto code = static_cast<uint16_t>(status_);
        return code >= 300 && code < 400;
    }
    
    /// Check if response is a client error (4xx)
    bool is_client_error() const noexcept {
        auto code = static_cast<uint16_t>(status_);
        return code >= 400 && code < 500;
    }
    
    /// Check if response is a server error (5xx)
    bool is_server_error() const noexcept {
        auto code = static_cast<uint16_t>(status_);
        return code >= 500 && code < 600;
    }
    
    /// Serialize response to string (HTTP/1.1 format)
    std::string serialize() const {
        std::string result;
        
        // Status line
        result += version_.empty() ? "HTTP/1.1" : version_;
        result += ' ';
        result += std::to_string(static_cast<uint16_t>(status_));
        result += ' ';
        result += status_reason(status_);
        result += "\r\n";
        
        // Headers
        result += headers_.serialize();
        
        // End of headers
        result += "\r\n";
        
        // Body
        if (!body_.empty()) {
            result += body_;
        }
        
        return result;
    }
    
    /// Create from parser
    static response from_parser(response_parser& parser) {
        response resp;
        resp.status_ = parser.get_status();
        resp.version_ = parser.version();
        resp.headers_ = parser.get_headers();
        resp.body_ = parser.body();
        return resp;
    }
    
    // Convenience factory methods
    
    /// Create OK response
    static response ok(std::string_view body = "", std::string_view content_type = mime::text_plain) {
        return response(status::ok, body, content_type);
    }
    
    /// Create JSON response
    static response json(std::string_view body) {
        return response(status::ok, body, mime::application_json);
    }
    
    /// Create HTML response
    static response html(std::string_view body) {
        return response(status::ok, body, mime::text_html);
    }
    
    /// Create not found response
    static response not_found(std::string_view body = "Not Found") {
        return response(status::not_found, body, mime::text_plain);
    }
    
    /// Create bad request response
    static response bad_request(std::string_view body = "Bad Request") {
        return response(status::bad_request, body, mime::text_plain);
    }
    
    /// Create internal server error response
    static response internal_error(std::string_view body = "Internal Server Error") {
        return response(status::internal_server_error, body, mime::text_plain);
    }
    
    /// Create redirect response
    static response redirect(std::string_view location, status s = status::found) {
        response resp(s);
        resp.set_header("Location", location);
        return resp;
    }
    
private:
    status status_ = status::ok;
    std::string version_ = "HTTP/1.1";
    headers headers_;
    std::string body_;
};

} // namespace elio::http
