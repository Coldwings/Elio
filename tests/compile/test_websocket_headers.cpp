#if defined(ELIO_TEST_WEBSOCKET_CLIENT)
#include <elio/http/websocket_client.hpp>
#elif defined(ELIO_TEST_WEBSOCKET_SERVER)
#include <elio/http/websocket_server.hpp>
#elif defined(ELIO_TEST_WEBSOCKET_CLIENT_SERVER)
#include <elio/http/websocket_client.hpp>
#include <elio/http/websocket_server.hpp>
#elif defined(ELIO_TEST_WEBSOCKET_SERVER_CLIENT)
#include <elio/http/websocket_server.hpp>
#include <elio/http/websocket_client.hpp>
#else
#error Select one standalone header or include order
#endif

#include <concepts>
#include <utility>

using elio::http::websocket::connection_state;
static_assert(static_cast<int>(connection_state::connecting) == 0);
static_assert(static_cast<int>(connection_state::open) == 1);
static_assert(static_cast<int>(connection_state::closing) == 2);
static_assert(static_cast<int>(connection_state::closed) == 3);

#if !defined(ELIO_TEST_WEBSOCKET_SERVER)
static_assert(std::same_as<decltype(std::declval<const elio::http::websocket::ws_client&>().state()),
    connection_state>);
#endif
#if !defined(ELIO_TEST_WEBSOCKET_CLIENT)
static_assert(std::same_as<decltype(std::declval<const elio::http::websocket::ws_connection&>().state()),
    connection_state>);
#endif

int main() { return 0; }
