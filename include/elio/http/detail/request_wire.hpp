#pragma once

#include <elio/http/detail/proxy_profile.hpp>
#include <elio/http/http_message.hpp>

#include <stdexcept>
#include <string>

namespace elio::http::detail {

// Project only the request line and headers. Request bodies remain borrowed by
// the enclosing exchange; route selection must not copy or mutate them.
struct request_wire_view {
    static bool valid_authority(const url& target) noexcept {
        return valid_proxy_host(target.host);
    }
    static std::string serialize(const request& req, const url& target,
            route_mode mode, const proxy_profile* proxy) {
        if (mode != route_mode::direct && !valid_authority(target))
            throw std::invalid_argument("Invalid proxy target authority");
        auto fields = req.get_headers();
        // Generic caller headers never select a hop credential domain, and a
        // CONNECT tunnel's inner request must not carry proxy credentials.
        fields.remove("Proxy-Authorization");
        auto wire_target = req.path_with_query();
        if (mode == route_mode::forward_proxy) {
            if (!proxy || target.scheme != "http")
                throw std::invalid_argument("Invalid HTTP forward proxy route");
            const bool server_options = req.path() == "*" &&
                req.get_method() == method::OPTIONS && req.query().empty();
            if (!server_options && ((!req.path().empty() && req.path().front() != '/') ||
                       req.path().find('#') != std::string_view::npos ||
                       req.query().find('#') != std::string_view::npos)) {
                throw std::invalid_argument("HTTP forward proxy requires an origin-form path");
            }
            if (!server_options) wire_target = "http://" + target.host_authority() + wire_target;
            fields.set("Host", target.host_authority());
            if (!proxy->authorization.empty())
                fields.set("Proxy-Authorization", proxy->authorization);
        }
        return req.serialize_headers_for(wire_target, fields);
    }
};

} // namespace elio::http::detail
