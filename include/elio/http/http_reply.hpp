#pragma once

#include <elio/http/http_message.hpp>
#include <elio/http/http_streaming_response.hpp>
#include <elio/http/http_tunnel_response.hpp>

#include <variant>

namespace elio::http {

using reply = std::variant<response, streaming_response, tunnel_response>;

} // namespace elio::http
