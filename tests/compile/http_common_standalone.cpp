// Deliberately include no umbrella or standard header first: this public
// header must declare its fixed-width integer dependencies independently.
#include <elio/http/http_common.hpp>

static_assert(static_cast<uint16_t>(elio::http::status::ok) == 200);
static_assert(elio::http::status_reason(elio::http::status::ok) == "OK");
