#pragma once

#include <elio/http/http_response_head.hpp>
#include <cerrno>
#include <limits>

namespace elio::http {

enum class response_body_kind { complete, streaming };
enum class response_transfer { automatic, close_delimited };
enum class response_framing { none, content_length, chunked, close_delimited };

struct body_description {
    response_body_kind kind = response_body_kind::complete;
    std::optional<uint64_t> length = uint64_t{0};
    response_transfer transfer = response_transfer::automatic;
};

struct response_plan {
    std::string header_block;
    response_framing framing = response_framing::none;
    std::optional<uint64_t> expected_body_bytes;
    bool invoke_producer = false;
    bool reusable = false;
    int error = 0;

    bool success() const noexcept { return error == 0; }
};

namespace detail {
inline bool response_plan_length(std::string_view text, uint64_t& value) noexcept {
    text = trim_ows(text);
    if (text.empty()) return false;
    value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        const auto digit = static_cast<uint64_t>(c - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}

inline bool response_plan_connection_close(std::string_view value) noexcept {
    while (!value.empty()) {
        const auto comma = value.find(',');
        if (ascii_iequals(trim_ows(value.substr(0, comma)), "close")) return true;
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    return false;
}
} // namespace detail

/// Validate and select framing before the first final-response byte is sent.
/// Allocation failure may throw; invalid response descriptions return EINVAL.
inline response_plan prepare_response(const response_head& head,
                                      body_description body,
                                      method request_method,
                                      std::string_view request_version,
                                      bool allow_reuse) {
    const auto invalid = [] {
        response_plan result;
        result.error = EINVAL;
        return result;
    };
    if (request_version != "HTTP/1.0" && request_version != "HTTP/1.1") return invalid();
    auto version = head.version();
    if (version.empty()) version = request_version;
    if (version != "HTTP/1.0" && version != "HTTP/1.1") return invalid();
    if (request_version == "HTTP/1.0") version = "HTTP/1.0";
    if (body.kind == response_body_kind::complete && !body.length) return invalid();

    const auto code = head.status_code();
    if (code < 200 || code > 599 ||
        (request_method == method::CONNECT && code < 300)) return invalid();
    const auto& source_headers = head.get_headers();
    if (source_headers.contains("Transfer-Encoding")) return invalid();
    std::optional<uint64_t> manual_length;
    if (source_headers.contains("Content-Length")) {
        uint64_t length = 0;
        // headers already coalesces identical Content-Length field lines.
        // Reject lists and all remaining ambiguity, not that lost wire history.
        if (source_headers.get_all("Content-Length").size() != 1 ||
            !detail::response_plan_length(source_headers.get("Content-Length"), length)) {
            return invalid();
        }
        manual_length = length;
    }

    response_plan result;
    std::optional<uint64_t> wire_length;
    const bool bodyless = request_method == method::HEAD || code == 204 || code == 205 || code == 304;
    if (bodyless) {
        // Status rules precede HEAD and producer configuration. Representation
        // metadata never becomes an obligation to send bytes for these cases.
        result.expected_body_bytes = uint64_t{0};
        if (code == 205) {
            wire_length = uint64_t{0};
        } else if (code == 304) {
            wire_length = head.representation_length().has_value()
                ? head.representation_length() : manual_length;
        } else if (code != 204) {
            wire_length = head.representation_length().has_value()
                ? head.representation_length() : body.length;
        }
        if (code != 204 && manual_length && manual_length != wire_length) return invalid();
    } else {
        if (body.transfer == response_transfer::close_delimited) {
            if (body.length || manual_length) return invalid();
            result.framing = response_framing::close_delimited;
        } else if (body.length) {
            if (manual_length && manual_length != body.length) return invalid();
            wire_length = body.length;
            result.framing = response_framing::content_length;
        } else {
            if (manual_length) return invalid();
            result.framing = version == "HTTP/1.1"
                ? response_framing::chunked : response_framing::close_delimited;
        }
        result.expected_body_bytes = body.length;
        result.invoke_producer = body.kind == response_body_kind::streaming;
    }

    auto wire_headers = source_headers;
    wire_headers.remove("Content-Length");
    if (wire_length) wire_headers.set("Content-Length", std::to_string(*wire_length));
    if (result.framing == response_framing::chunked) wire_headers.set("Transfer-Encoding", "chunked");
    result.reusable = allow_reuse && result.framing != response_framing::close_delimited &&
        !detail::response_plan_connection_close(source_headers.get("Connection"));
    if (!result.reusable) wire_headers.set("Connection", "close");
    else if (version == "HTTP/1.0") wire_headers.set("Connection", "keep-alive");

    result.header_block = std::string(version) + ' ' + std::to_string(code) + ' ' +
        std::string(status_reason(head.get_status())) + "\r\n" + wire_headers.serialize() + "\r\n";
    return result;
}

} // namespace elio::http
