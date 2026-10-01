#pragma once

#include <elio/http/proxy_config.hpp>
#include <elio/http/detail/route_plan.hpp>
#include <elio/http/http_common.hpp>

#include <openssl/evp.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace elio::http::detail {

struct proxy_profile {
    route_endpoint endpoint;
    uint64_t auth_domain = 0;
    std::string authorization;
    proxy_connect_limits limits;
};

inline std::string proxy_basic_authorization(const proxy_basic_credentials& credentials) {
    constexpr size_t max_credentials_bytes = 4096;
    if (credentials.username.size() >= max_credentials_bytes ||
        credentials.password.size() > max_credentials_bytes - credentials.username.size() - 1)
        throw std::invalid_argument("HTTP proxy credentials exceed their bound");
    const auto has_control = [](std::string_view value) {
        return std::any_of(value.begin(), value.end(), [](unsigned char c) {
            return c < 0x20 || c == 0x7f;
        });
    };
    if (credentials.username.find(':') != std::string::npos ||
        has_control(credentials.username) || has_control(credentials.password))
        throw std::invalid_argument("Invalid HTTP proxy Basic credentials");
    auto combined = credentials.username + ":" + credentials.password;
    std::string encoded(4 * ((combined.size() + 2) / 3) + 1, '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
        reinterpret_cast<const unsigned char*>(combined.data()), static_cast<int>(combined.size()));
    if (written < 0) throw std::runtime_error("HTTP proxy credential encoding failed");
    encoded.resize(static_cast<size_t>(written));
    return "Basic " + encoded;
}

inline std::shared_ptr<const proxy_profile> freeze_proxy_profile(const http_proxy_config& config) {
    // The shared parser stores empty and absent URI decorations alike. Reject
    // their delimiters too: the endpoint policy accepts only an authority.
    if (config.endpoint.size() > 8192 || config.endpoint.find("://") == std::string::npos ||
        config.endpoint.find_first_of("@?#") != std::string::npos)
        throw std::invalid_argument("HTTP proxy requires an explicit bounded endpoint URI");
    const auto parsed = url::parse(config.endpoint);
    if (!parsed || parsed->scheme != "http" || !parsed->userinfo.empty() ||
        parsed->path != "/" || !parsed->query.empty() || !parsed->fragment.empty())
        throw std::invalid_argument("HTTP proxy endpoint must be a plain HTTP authority");
    auto profile = std::make_shared<proxy_profile>();
    profile->endpoint = route_endpoint::from(parsed->host, parsed->effective_port());
    profile->limits = config.connect_limits;
    if (config.basic_auth) {
        profile->authorization = proxy_basic_authorization(*config.basic_auth);
        profile->auth_domain = new_route_domain();
    }
    return profile;
}

} // namespace elio::http::detail
