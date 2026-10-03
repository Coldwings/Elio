#pragma once

#include <cerrno>
#include <system_error>
#include <variant>

namespace elio::http {

enum class client_stage {
    target, resolve, acquire, connect, tls, request, headers, body, framing,
    proxy_connect, proxy_tls
};

/// Owned, bounded metadata: no URLs, credentials, diagnostics, or views.
/// Operational failures use positive errno codes in the generic category.
struct client_error {
    std::error_code code;
    client_stage stage;
};

template<typename T>
using client_result = std::variant<T, client_error>;

namespace detail {

inline client_error make_client_error(int error, client_stage stage) noexcept {
    return {std::error_code(error > 0 ? error : EIO, std::generic_category()), stage};
}

} // namespace detail
} // namespace elio::http
