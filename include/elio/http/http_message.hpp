#pragma once

#include <elio/http/http_common.hpp>
#include <elio/http/http_parser.hpp>
#include <elio/http/http_response_head.hpp>
#include <elio/http/http_response_plan.hpp>

#include <string>
#include <string_view>

namespace elio::http {

/// HTTP request
class request {
public:
    request() = default;
    
    /// Create a request with method and path
    request(method m, std::string_view path)
        : method_(m) {
        set_path(path);
    }
    
    /// Get/set method
    method get_method() const noexcept { return method_; }
    void set_method(method m) noexcept { method_ = m; }
    
    /// Get/set path
    std::string_view path() const noexcept { return path_; }
    void set_path(std::string_view p) {
        if (!detail::is_valid_request_target_component(p)) {
            throw std::invalid_argument("elio::http::request: invalid path");
        }
        path_ = p;
    }
    
    /// Get/set query string
    std::string_view query() const noexcept { return query_; }
    void set_query(std::string_view q) {
        if (!detail::is_valid_request_target_component(q)) {
            throw std::invalid_argument("elio::http::request: invalid query");
        }
        query_ = q;
    }
    
    /// Get/set HTTP version
    std::string_view version() const noexcept { return version_; }
    void set_version(std::string_view v) {
        detail::validate_http_version(v);
        version_ = v;
    }
    
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
    void set_body(std::string_view b) { body_ = b; headers_.set_content_length(body_.size()); }
    void set_body(std::string&& b) { body_ = std::move(b); headers_.set_content_length(body_.size()); }
    
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

    /// Enable/disable the Expect: 100-continue handshake for this request.
    /// When enabled and the request has a body, http::client sends the
    /// headers first, then waits (bounded by
    /// client_config::expect_continue_timeout) for an interim 100 Continue
    /// before sending the body. The flag only controls client sending; it
    /// is not derived from a parsed Expect header.
    ///
    /// This setter owns the Expect header: enabling overwrites any existing
    /// Expect value, and disabling removes the header outright, including a
    /// caller-set custom value. The header is only serialized when the
    /// request actually has a body — RFC 9110 §10.1.1 forbids sending
    /// Expect: 100-continue without content.
    void set_expect_continue(bool on = true) {
        expect_continue_ = on;
        if (on) {
            headers_.set("Expect", "100-continue");
        } else {
            headers_.remove("Expect");
        }
    }

    /// Whether Expect: 100-continue sending is enabled
    bool expect_continue() const noexcept { return expect_continue_; }

    /// Parse query string parameters
    std::unordered_map<std::string, std::string> query_params() const {
        return parse_query_string(query_);
    }

    /// Serialize request line and headers only (HTTP/1.1 format, no body).
    /// serialize() shares this and appends the body.
    std::string serialize_headers() const {
        std::string result;

        // Request line
        result += method_to_string(method_);
        result += ' ';
        auto target = path_with_query();
        detail::validate_request_target(target);
        result += target;
        result += ' ';
        std::string_view version =
            version_.empty() ? std::string_view("HTTP/1.1")
                             : std::string_view(version_);
        detail::validate_http_version(version);
        result += version;
        result += "\r\n";

        // Headers. A bodyless request must not advertise
        // Expect: 100-continue (RFC 9110 §10.1.1); drop the setter-managed
        // header only for that case.
        if (expect_continue_ && body_.empty()) {
            auto no_expect = headers_;
            no_expect.remove("Expect");
            result += no_expect.serialize();
        } else {
            result += headers_.serialize();
        }

        // End of headers
        result += "\r\n";

        return result;
    }

    /// Serialize request to string (HTTP/1.1 format)
    std::string serialize() const {
        auto result = serialize_headers();
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
    bool expect_continue_ = false;
};

/// HTTP response
class response : public response_head {
public:
    response() = default;
    
    /// Create a response with status
    explicit response(status s) : response_head(s) {}
    
    /// Create a response with status and body
    response(status s, std::string_view body, std::string_view content_type = mime::text_plain)
        : response_head(s), body_(body) {
        get_headers().set_content_type(content_type);
    }
    
    /// Body storage only; framing length is derived when preparing output.
    /// Explicit Content-Length remains an assertion, not setter-owned state.
    std::string_view body() const noexcept { return body_; }
    void set_body(std::string_view b) { 
        body_ = b; 
    }
    void set_body(std::string&& b) { 
        body_ = std::move(b);
    }
    
    /// Materialize a final response using the same framing preflight as the
    /// server. Invalid framing assertions throw std::invalid_argument.
    /// Received transfer-coded messages must be explicitly normalized by the
    /// caller (remove Transfer-Encoding) before serializing their decoded body.
    std::string serialize() const { return serialize(method::GET); }

    /// HEAD and bodyless statuses emit headers only. Informational responses,
    /// upgrades and successful CONNECT use a separate headers-only path.
    std::string serialize(method request_method) const {
        const auto code = status_code();
        if ((code >= 100 && code < 200) ||
            (request_method == method::CONNECT && code >= 200 && code < 300)) {
            return serialize_protocol_headers();
        }
        const auto wire_version = version().empty() ? std::string_view("HTTP/1.1") : version();
        auto plan = prepare_response(*this,
            {response_body_kind::complete, body_.size(), response_transfer::automatic},
            request_method, wire_version, get_headers().keep_alive(wire_version));
        if (!plan.success()) throw std::invalid_argument("Invalid outbound HTTP response framing");
        if (plan.framing != response_framing::none) plan.header_block += body_;
        return std::move(plan.header_block);
    }

private:
    std::string serialize_protocol_headers() const {
        const auto wire_version = version().empty() ? std::string_view("HTTP/1.1") : version();
        if (wire_version != "HTTP/1.0" && wire_version != "HTTP/1.1") {
            throw std::invalid_argument("Unsupported outbound HTTP version");
        }
        auto wire_headers = get_headers();
        // This branch never participates in ordinary final-body framing:
        // 1xx/101 and successful CONNECT prohibit both length and transfer coding.
        wire_headers.remove("Content-Length");
        wire_headers.remove("Transfer-Encoding");
        return std::string(wire_version) + ' ' + std::to_string(status_code()) + ' ' +
            std::string(status_reason(get_status())) + "\r\n" + wire_headers.serialize() + "\r\n";
    }

public:
    
    /// Preserve received metadata and decoded body. Transfer-Encoding remains
    /// received metadata, not permission to emit decoded bytes with that coding.
    static response from_parser(response_parser& parser) {
        response resp;
        resp.set_status(parser.get_status());
        resp.set_version(parser.version());
        resp.get_headers() = parser.get_headers();
        resp.body_ = parser.body();
        return resp;
    }

    /// Materialize a received response without regenerating its wire headers.
    /// The caller supplies the decoded body; no transfer decoding happens here.
    static response from_decoder(const response_decoder& decoder, std::string body) {
        response resp;
        resp.set_status(decoder.get_status());
        resp.set_version(decoder.version());
        resp.get_headers() = decoder.get_headers();
        resp.body_ = std::move(body);
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
    std::string body_;
};

} // namespace elio::http
