#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <optional>
#include <algorithm>
#include <charconv>
#include <cctype>

namespace elio::http {

/// HTTP methods
enum class method {
    GET,
    HEAD,
    POST,
    PUT,
    DELETE_,  // DELETE is a C++ keyword
    CONNECT,
    OPTIONS,
    TRACE,
    PATCH
};

/// Convert method enum to string
inline constexpr std::string_view method_to_string(method m) noexcept {
    switch (m) {
        case method::GET:      return "GET";
        case method::HEAD:     return "HEAD";
        case method::POST:     return "POST";
        case method::PUT:      return "PUT";
        case method::DELETE_:  return "DELETE";
        case method::CONNECT:  return "CONNECT";
        case method::OPTIONS:  return "OPTIONS";
        case method::TRACE:    return "TRACE";
        case method::PATCH:    return "PATCH";
    }
    return "UNKNOWN";
}

/// Convert string to method enum
inline std::optional<method> string_to_method(std::string_view str) noexcept {
    if (str == "GET")     return method::GET;
    if (str == "HEAD")    return method::HEAD;
    if (str == "POST")    return method::POST;
    if (str == "PUT")     return method::PUT;
    if (str == "DELETE")  return method::DELETE_;
    if (str == "CONNECT") return method::CONNECT;
    if (str == "OPTIONS") return method::OPTIONS;
    if (str == "TRACE")   return method::TRACE;
    if (str == "PATCH")   return method::PATCH;
    return std::nullopt;
}

/// HTTP status codes
enum class status : uint16_t {
    // 1xx Informational
    continue_ = 100,
    switching_protocols = 101,
    processing = 102,
    early_hints = 103,
    
    // 2xx Success
    ok = 200,
    created = 201,
    accepted = 202,
    non_authoritative_information = 203,
    no_content = 204,
    reset_content = 205,
    partial_content = 206,
    multi_status = 207,
    already_reported = 208,
    im_used = 226,
    
    // 3xx Redirection
    multiple_choices = 300,
    moved_permanently = 301,
    found = 302,
    see_other = 303,
    not_modified = 304,
    use_proxy = 305,
    temporary_redirect = 307,
    permanent_redirect = 308,
    
    // 4xx Client Error
    bad_request = 400,
    unauthorized = 401,
    payment_required = 402,
    forbidden = 403,
    not_found = 404,
    method_not_allowed = 405,
    not_acceptable = 406,
    proxy_authentication_required = 407,
    request_timeout = 408,
    conflict = 409,
    gone = 410,
    length_required = 411,
    precondition_failed = 412,
    payload_too_large = 413,
    uri_too_long = 414,
    unsupported_media_type = 415,
    range_not_satisfiable = 416,
    expectation_failed = 417,
    im_a_teapot = 418,
    misdirected_request = 421,
    unprocessable_entity = 422,
    locked = 423,
    failed_dependency = 424,
    too_early = 425,
    upgrade_required = 426,
    precondition_required = 428,
    too_many_requests = 429,
    request_header_fields_too_large = 431,
    unavailable_for_legal_reasons = 451,
    
    // 5xx Server Error
    internal_server_error = 500,
    not_implemented = 501,
    bad_gateway = 502,
    service_unavailable = 503,
    gateway_timeout = 504,
    http_version_not_supported = 505,
    variant_also_negotiates = 506,
    insufficient_storage = 507,
    loop_detected = 508,
    not_extended = 510,
    network_authentication_required = 511
};

/// Get reason phrase for status code
inline constexpr std::string_view status_reason(status s) noexcept {
    switch (s) {
        case status::continue_: return "Continue";
        case status::switching_protocols: return "Switching Protocols";
        case status::processing: return "Processing";
        case status::early_hints: return "Early Hints";
        case status::ok: return "OK";
        case status::created: return "Created";
        case status::accepted: return "Accepted";
        case status::non_authoritative_information: return "Non-Authoritative Information";
        case status::no_content: return "No Content";
        case status::reset_content: return "Reset Content";
        case status::partial_content: return "Partial Content";
        case status::multi_status: return "Multi-Status";
        case status::already_reported: return "Already Reported";
        case status::im_used: return "IM Used";
        case status::multiple_choices: return "Multiple Choices";
        case status::moved_permanently: return "Moved Permanently";
        case status::found: return "Found";
        case status::see_other: return "See Other";
        case status::not_modified: return "Not Modified";
        case status::use_proxy: return "Use Proxy";
        case status::temporary_redirect: return "Temporary Redirect";
        case status::permanent_redirect: return "Permanent Redirect";
        case status::bad_request: return "Bad Request";
        case status::unauthorized: return "Unauthorized";
        case status::payment_required: return "Payment Required";
        case status::forbidden: return "Forbidden";
        case status::not_found: return "Not Found";
        case status::method_not_allowed: return "Method Not Allowed";
        case status::not_acceptable: return "Not Acceptable";
        case status::proxy_authentication_required: return "Proxy Authentication Required";
        case status::request_timeout: return "Request Timeout";
        case status::conflict: return "Conflict";
        case status::gone: return "Gone";
        case status::length_required: return "Length Required";
        case status::precondition_failed: return "Precondition Failed";
        case status::payload_too_large: return "Payload Too Large";
        case status::uri_too_long: return "URI Too Long";
        case status::unsupported_media_type: return "Unsupported Media Type";
        case status::range_not_satisfiable: return "Range Not Satisfiable";
        case status::expectation_failed: return "Expectation Failed";
        case status::im_a_teapot: return "I'm a teapot";
        case status::misdirected_request: return "Misdirected Request";
        case status::unprocessable_entity: return "Unprocessable Entity";
        case status::locked: return "Locked";
        case status::failed_dependency: return "Failed Dependency";
        case status::too_early: return "Too Early";
        case status::upgrade_required: return "Upgrade Required";
        case status::precondition_required: return "Precondition Required";
        case status::too_many_requests: return "Too Many Requests";
        case status::request_header_fields_too_large: return "Request Header Fields Too Large";
        case status::unavailable_for_legal_reasons: return "Unavailable For Legal Reasons";
        case status::internal_server_error: return "Internal Server Error";
        case status::not_implemented: return "Not Implemented";
        case status::bad_gateway: return "Bad Gateway";
        case status::service_unavailable: return "Service Unavailable";
        case status::gateway_timeout: return "Gateway Timeout";
        case status::http_version_not_supported: return "HTTP Version Not Supported";
        case status::variant_also_negotiates: return "Variant Also Negotiates";
        case status::insufficient_storage: return "Insufficient Storage";
        case status::loop_detected: return "Loop Detected";
        case status::not_extended: return "Not Extended";
        case status::network_authentication_required: return "Network Authentication Required";
        default: return "Unknown";
    }
}

/// Case-insensitive string comparison for headers
struct case_insensitive_hash {
    size_t operator()(std::string_view s) const noexcept {
        size_t hash = 0;
        for (char c : s) {
            hash = hash * 31 + static_cast<size_t>(std::tolower(static_cast<unsigned char>(c)));
        }
        return hash;
    }
};

struct case_insensitive_equal {
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) != 
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    }
};

/// HTTP headers collection (case-insensitive keys)
class headers {
public:
    using map_type = std::unordered_map<std::string, std::string, case_insensitive_hash, case_insensitive_equal>;
    using iterator = map_type::iterator;
    using const_iterator = map_type::const_iterator;
    
    headers() = default;
    
    /// Set a header (overwrites existing)
    void set(std::string_view name, std::string_view value) {
        headers_[std::string(name)] = std::string(value);
    }
    
    /// Add a header (appends with comma if exists)
    void add(std::string_view name, std::string_view value) {
        auto it = headers_.find(std::string(name));
        if (it != headers_.end()) {
            it->second += ", ";
            it->second += value;
        } else {
            headers_[std::string(name)] = std::string(value);
        }
    }
    
    /// Get a header value (or empty if not found)
    std::string_view get(std::string_view name) const {
        auto it = headers_.find(std::string(name));
        if (it != headers_.end()) {
            return it->second;
        }
        return {};
    }
    
    /// Check if header exists
    bool contains(std::string_view name) const {
        return headers_.find(std::string(name)) != headers_.end();
    }
    
    /// Remove a header
    void remove(std::string_view name) {
        headers_.erase(std::string(name));
    }
    
    /// Get Content-Length header value
    std::optional<size_t> content_length() const {
        auto val = get("Content-Length");
        if (val.empty()) return std::nullopt;
        size_t len = 0;
        auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), len);
        if (ec == std::errc{}) return len;
        return std::nullopt;
    }
    
    /// Set Content-Length header
    void set_content_length(size_t len) {
        set("Content-Length", std::to_string(len));
    }
    
    /// Get Content-Type header
    std::string_view content_type() const {
        return get("Content-Type");
    }
    
    /// Set Content-Type header
    void set_content_type(std::string_view type) {
        set("Content-Type", type);
    }
    
    /// Check if Transfer-Encoding is chunked
    bool is_chunked() const {
        auto te = get("Transfer-Encoding");
        return te.find("chunked") != std::string_view::npos;
    }
    
    /// Check if connection should be kept alive
    bool keep_alive(std::string_view http_version = "1.1") const {
        auto conn = get("Connection");
        if (!conn.empty()) {
            // Case-insensitive check
            std::string lower;
            lower.reserve(conn.size());
            for (char c : conn) {
                lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (lower.find("close") != std::string::npos) return false;
            if (lower.find("keep-alive") != std::string::npos) return true;
        }
        // Default: HTTP/1.1 keeps alive, HTTP/1.0 closes
        return http_version == "1.1" || http_version == "HTTP/1.1";
    }
    
    /// Clear all headers
    void clear() {
        headers_.clear();
    }
    
    /// Get number of headers
    size_t size() const noexcept {
        return headers_.size();
    }
    
    /// Check if empty
    bool empty() const noexcept {
        return headers_.empty();
    }
    
    /// Iterators
    iterator begin() { return headers_.begin(); }
    iterator end() { return headers_.end(); }
    const_iterator begin() const { return headers_.begin(); }
    const_iterator end() const { return headers_.end(); }
    const_iterator cbegin() const { return headers_.cbegin(); }
    const_iterator cend() const { return headers_.cend(); }
    
    /// Serialize headers to string
    std::string serialize() const {
        std::string result;
        for (const auto& [name, value] : headers_) {
            result += name;
            result += ": ";
            result += value;
            result += "\r\n";
        }
        return result;
    }
    
private:
    map_type headers_;
};

/// URL components
struct url {
    std::string scheme;     ///< http or https
    std::string host;       ///< hostname
    uint16_t port = 0;      ///< port (0 = default)
    std::string path;       ///< path including leading /
    std::string query;      ///< query string (without ?)
    std::string fragment;   ///< fragment (without #)
    std::string userinfo;   ///< username:password
    
    /// Get the path with query string
    std::string path_with_query() const {
        if (query.empty()) return path.empty() ? "/" : path;
        return (path.empty() ? "/" : path) + "?" + query;
    }
    
    /// Get the authority (host:port)
    std::string authority() const {
        std::string result;
        if (!userinfo.empty()) {
            result = userinfo + "@";
        }
        result += host;
        if (port != 0 && port != default_port()) {
            result += ":" + std::to_string(port);
        }
        return result;
    }
    
    /// Get the full URL
    std::string to_string() const {
        std::string result = scheme + "://";
        result += authority();
        result += path_with_query();
        if (!fragment.empty()) {
            result += "#" + fragment;
        }
        return result;
    }
    
    /// Get effective port
    uint16_t effective_port() const {
        return port != 0 ? port : default_port();
    }
    
    /// Get default port for scheme
    uint16_t default_port() const {
        if (scheme == "https") return 443;
        if (scheme == "http") return 80;
        return 80;
    }
    
    /// Check if HTTPS
    bool is_secure() const {
        return scheme == "https";
    }
    
    /// Parse URL from string
    static std::optional<url> parse(std::string_view str) {
        url result;
        
        // Find scheme
        auto scheme_end = str.find("://");
        if (scheme_end == std::string_view::npos) {
            // No scheme, assume http
            result.scheme = "http";
        } else {
            result.scheme = str.substr(0, scheme_end);
            str = str.substr(scheme_end + 3);
        }
        
        // Convert scheme to lowercase
        for (char& c : result.scheme) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        
        // Find fragment
        auto frag_pos = str.find('#');
        if (frag_pos != std::string_view::npos) {
            result.fragment = str.substr(frag_pos + 1);
            str = str.substr(0, frag_pos);
        }
        
        // Find query
        auto query_pos = str.find('?');
        if (query_pos != std::string_view::npos) {
            result.query = str.substr(query_pos + 1);
            str = str.substr(0, query_pos);
        }
        
        // Find path
        auto path_pos = str.find('/');
        if (path_pos != std::string_view::npos) {
            result.path = str.substr(path_pos);
            str = str.substr(0, path_pos);
        } else {
            result.path = "/";
        }
        
        // Find userinfo
        auto at_pos = str.find('@');
        if (at_pos != std::string_view::npos) {
            result.userinfo = str.substr(0, at_pos);
            str = str.substr(at_pos + 1);
        }
        
        // Parse host:port
        // Handle IPv6 addresses [...]
        if (!str.empty() && str[0] == '[') {
            auto bracket_end = str.find(']');
            if (bracket_end == std::string_view::npos) {
                return std::nullopt;  // Invalid IPv6
            }
            result.host = str.substr(1, bracket_end - 1);
            str = str.substr(bracket_end + 1);
            if (!str.empty() && str[0] == ':') {
                str = str.substr(1);
                uint16_t port = 0;
                auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), port);
                if (ec == std::errc{}) {
                    result.port = port;
                }
            }
        } else {
            auto colon_pos = str.rfind(':');
            if (colon_pos != std::string_view::npos) {
                result.host = str.substr(0, colon_pos);
                auto port_str = str.substr(colon_pos + 1);
                uint16_t port = 0;
                auto [ptr, ec] = std::from_chars(port_str.data(), port_str.data() + port_str.size(), port);
                if (ec == std::errc{}) {
                    result.port = port;
                }
            } else {
                result.host = str;
            }
        }
        
        if (result.host.empty()) {
            return std::nullopt;
        }
        
        return result;
    }
};

/// URL-encode a string
inline std::string url_encode(std::string_view str) {
    std::string result;
    result.reserve(str.size() * 3);  // Worst case
    
    static const char hex[] = "0123456789ABCDEF";
    
    for (unsigned char c : str) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            result += static_cast<char>(c);
        } else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 0x0F];
        }
    }
    
    return result;
}

/// URL-decode a string
inline std::string url_decode(std::string_view str) {
    std::string result;
    result.reserve(str.size());
    
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            int hi = 0, lo = 0;
            auto [p1, e1] = std::from_chars(&str[i + 1], &str[i + 2], hi, 16);
            auto [p2, e2] = std::from_chars(&str[i + 2], &str[i + 3], lo, 16);
            if (e1 == std::errc{} && e2 == std::errc{}) {
                result += static_cast<char>((hi << 4) | lo);
                i += 2;
            } else {
                result += str[i];
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    
    return result;
}

/// Parse query string into key-value pairs
inline std::unordered_map<std::string, std::string> parse_query_string(std::string_view query) {
    std::unordered_map<std::string, std::string> result;
    
    while (!query.empty()) {
        auto amp_pos = query.find('&');
        auto pair = (amp_pos == std::string_view::npos) ? query : query.substr(0, amp_pos);
        
        auto eq_pos = pair.find('=');
        if (eq_pos != std::string_view::npos) {
            auto key = url_decode(pair.substr(0, eq_pos));
            auto value = url_decode(pair.substr(eq_pos + 1));
            result[key] = value;
        } else if (!pair.empty()) {
            result[url_decode(pair)] = "";
        }
        
        if (amp_pos == std::string_view::npos) break;
        query = query.substr(amp_pos + 1);
    }
    
    return result;
}

/// Common MIME types
namespace mime {
    inline constexpr std::string_view text_plain = "text/plain";
    inline constexpr std::string_view text_html = "text/html";
    inline constexpr std::string_view text_css = "text/css";
    inline constexpr std::string_view text_javascript = "text/javascript";
    inline constexpr std::string_view application_json = "application/json";
    inline constexpr std::string_view application_xml = "application/xml";
    inline constexpr std::string_view application_octet_stream = "application/octet-stream";
    inline constexpr std::string_view application_form_urlencoded = "application/x-www-form-urlencoded";
    inline constexpr std::string_view multipart_form_data = "multipart/form-data";
    inline constexpr std::string_view image_png = "image/png";
    inline constexpr std::string_view image_jpeg = "image/jpeg";
    inline constexpr std::string_view image_gif = "image/gif";
    inline constexpr std::string_view image_svg = "image/svg+xml";
}

} // namespace elio::http
