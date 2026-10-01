#pragma once

#include <elio/http/proxy_config.hpp>
#include <elio/http/detail/route_plan.hpp>
#include <elio/http/http_common.hpp>

#include <openssl/evp.h>
#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace elio::http::detail {

inline bool valid_proxy_host(std::string_view host) noexcept {
    if (!is_valid_url_input(host)) return false;
    if (host.find(':') != std::string_view::npos) {
        std::array<char, INET6_ADDRSTRLEN> text{};
        if (host.size() >= text.size()) return false;
        std::copy(host.begin(), host.end(), text.begin());
        in6_addr address{};
        return ::inet_pton(AF_INET6, text.data(), &address) == 1;
    }
    for (size_t pos = 0; pos < host.size(); ++pos) {
        if (host[pos] == '%') {
            if (host.size() - pos < 3 || !connect_hex_digit(host[pos + 1]) ||
                !connect_hex_digit(host[pos + 2])) return false;
            pos += 2;
        } else if (!connect_host_char(host[pos])) {
            return false;
        }
    }
    return true;
}

inline bool valid_proxy_uri_authority(std::string_view source, const url& parsed) noexcept {
    if (!valid_proxy_host(parsed.host)) return false;
    // url::parse drops IP-literal brackets. Do not reinterpret a bracketed
    // non-IP host as a reg-name when projecting it onto the proxy wire.
    const auto scheme = source.find("://");
    if (scheme != std::string_view::npos) source.remove_prefix(scheme + 3);
    else if (source.starts_with("//")) source.remove_prefix(2);
    source = source.substr(0, source.find_first_of("/?#"));
    const auto userinfo = source.find('@');
    if (userinfo != std::string_view::npos) source.remove_prefix(userinfo + 1);
    return source.empty() || source.front() != '[' || parsed.host.find(':') != std::string::npos;
}

inline std::optional<char> proxy_unreserved_octet(std::string_view hex) noexcept {
    if (hex.size() != 2) return std::nullopt;
    unsigned int octet = 0;
    const auto* end = hex.data() + hex.size();
    const auto [parsed, error] = std::from_chars(hex.data(), end, octet, 16);
    const bool unreserved = (octet >= 'a' && octet <= 'z') ||
        (octet >= 'A' && octet <= 'Z') || (octet >= '0' && octet <= '9') ||
        octet == '-' || octet == '.' || octet == '_' || octet == '~';
    if (error != std::errc{} || parsed != end || !unreserved) return std::nullopt;
    return static_cast<char>(octet);
}

inline bool valid_proxy_tls_reference(std::string_view host) noexcept {
    if (!valid_proxy_host(host) || host.empty() || host.front() == '.') return false;
    for (size_t pos = 0; pos < host.size(); ++pos) {
        if (host[pos] != '%') continue;
        const auto octet = proxy_unreserved_octet(host.substr(pos + 1, 2));
        // OpenSSL interprets a leading dot as a subdomain reference, not an
        // exact destination. URI decoding must not widen origin authentication.
        if (!octet || (pos == 0 && *octet == '.')) return false;
        pos += 2;
    }
    return true;
}

inline std::string decoded_proxy_host(std::string_view host) {
    std::string decoded;
    decoded.reserve(host.size());
    for (size_t pos = 0; pos < host.size(); ++pos) {
        if (host[pos] != '%') {
            decoded.push_back(host[pos]);
            continue;
        }
        if (host.size() - pos < 3)
            throw std::invalid_argument("Invalid encoded proxy hostname");
        const auto octet = proxy_unreserved_octet(host.substr(pos + 1, 2));
        // Decode once, after URI splitting. Unsupported encoded octets must
        // not create a new delimiter, nested escape, or implicit IDNA policy.
        if (!octet)
            throw std::invalid_argument("Unsupported encoded proxy hostname octet");
        decoded.push_back(*octet);
        pos += 2;
    }
    return decoded;
}

inline std::string proxy_origin_tls_name(std::string_view host) {
    if (!valid_proxy_tls_reference(host))
        throw std::invalid_argument("Invalid proxy origin TLS reference name");
    return normalize_route_host(decoded_proxy_host(host));
}

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
        parsed->path != "/" || !parsed->query.empty() || !parsed->fragment.empty() ||
        !valid_proxy_uri_authority(config.endpoint, *parsed))
        throw std::invalid_argument("HTTP proxy endpoint must be a plain HTTP authority");
    auto profile = std::make_shared<proxy_profile>();
    profile->endpoint = route_endpoint::from(decoded_proxy_host(parsed->host), parsed->effective_port());
    profile->limits = config.connect_limits;
    if (config.basic_auth) {
        profile->authorization = proxy_basic_authorization(*config.basic_auth);
        profile->auth_domain = new_route_domain();
    }
    return profile;
}

} // namespace elio::http::detail
