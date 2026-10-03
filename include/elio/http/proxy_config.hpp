#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace elio::http {

struct proxy_basic_credentials {
    std::string username;
    std::string password;
};

struct proxy_connect_limits {
    size_t max_headers = 100;
    size_t max_header_size = 8192;
    size_t max_response_bytes = 64 * 1024;
    size_t max_informational_responses = 8;
    size_t max_read_ahead = 8192;
};

/// Explicit single HTTP proxy hop; no environment discovery or challenge replay.
/// Credential strings are caller-selected octets, not locale/charset converted.
struct http_proxy_config {
    std::string endpoint;
    std::optional<proxy_basic_credentials> basic_auth;
    proxy_connect_limits connect_limits;
};

} // namespace elio::http
