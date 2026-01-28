#pragma once

/// @file serve.hpp
/// @brief Server lifecycle utilities
///
/// This module provides utilities for running servers with graceful shutdown
/// on signal (e.g., SIGINT, SIGTERM).
///
/// Usage:
/// @code
/// coro::task<int> async_main(int argc, char* argv[]) {
///     http::router r;
///     r.get("/", hello_handler);
///
///     http::server srv(r);
///     co_await elio::serve(srv, net::ipv4_address(8080));
///
///     co_return 0;
/// }
///
/// ELIO_ASYNC_MAIN(async_main)
/// @endcode

#include <elio/coro/task.hpp>
#include <elio/signal/signalfd.hpp>
#include <elio/log/macros.hpp>

#include <csignal>
#include <initializer_list>
#include <type_traits>

namespace elio {

/// Default shutdown signals
inline constexpr std::initializer_list<int> default_shutdown_signals = {SIGINT, SIGTERM};

/// Wait for shutdown signals (SIGINT or SIGTERM)
///
/// This is a simple awaitable that blocks until a shutdown signal is received.
/// Useful for implementing custom server loops.
///
/// @param signals Signal set to wait for (defaults to SIGINT, SIGTERM)
/// @return signal_info about the received signal
///
/// Example:
/// @code
/// coro::task<void> server_main() {
///     // Start your server tasks...
///
///     auto sig = co_await wait_shutdown_signal();
///     ELIO_LOG_INFO("Received {}, shutting down...", sig.full_name());
///
///     // Stop your servers...
/// }
/// @endcode
inline coro::task<signal::signal_info> wait_shutdown_signal(
    std::initializer_list<int> signals = default_shutdown_signals)
{
    signal::signal_set sigs(signals);
    signal::signal_fd sigfd(sigs);

    auto info = co_await sigfd.wait();
    if (info) {
        co_return *info;
    }

    // Return empty info if wait failed
    co_return signal::signal_info{};
}

/// Serve a server until shutdown signal is received
///
/// This function runs the server's listen task and waits for a shutdown signal.
/// When a signal is received, it calls server.stop() and waits for the listen
/// task to complete.
///
/// @tparam Server Server type (must have stop() method)
/// @tparam ListenTask The awaitable returned by server.listen()
/// @param server Reference to the server (used to call stop())
/// @param listen_task The listen coroutine task
/// @param signals Signals to wait for shutdown (defaults to SIGINT, SIGTERM)
///
/// Example:
/// @code
/// coro::task<int> async_main(int argc, char* argv[]) {
///     http::router r;
///     r.get("/", handler);
///
///     http::server srv(r);
///     co_await serve(srv, srv.listen(net::ipv4_address(8080)));
///
///     co_return 0;
/// }
///
/// ELIO_ASYNC_MAIN(async_main)
/// @endcode
template<typename Server, typename ListenTask>
coro::task<void> serve(Server& server, ListenTask listen_task,
                       std::initializer_list<int> signals = default_shutdown_signals)
{
    // Set up signal handling
    signal::signal_set sigs(signals);
    signal::signal_fd sigfd(sigs);

    // Spawn the listen task
    auto listen_handle = std::move(listen_task).spawn();

    // Wait for shutdown signal
    auto info = co_await sigfd.wait();
    if (info) {
        ELIO_LOG_INFO("Received SIG{}, initiating shutdown...", info->name());
    }

    // Stop the server
    server.stop();

    // Wait for listen task to complete
    co_await listen_handle;

    ELIO_LOG_INFO("Server stopped");
    co_return;
}

/// Serve a server with TLS until shutdown signal is received
///
/// Convenience overload for TLS servers.
///
/// @tparam Server Server type (must have stop() method)
/// @tparam ListenTask The awaitable returned by server.listen_tls()
/// @param server Reference to the server
/// @param listen_task The TLS listen coroutine task
/// @param signals Signals to wait for shutdown
///
/// Example:
/// @code
/// coro::task<int> async_main(int argc, char* argv[]) {
///     http::router r;
///     r.get("/", handler);
///
///     http::server srv(r);
///     auto tls_ctx = tls::tls_context::make_server("cert.pem", "key.pem");
///
///     co_await serve(srv, srv.listen_tls(addr, tls_ctx));
///
///     co_return 0;
/// }
/// @endcode
// Note: This is the same function as above, the template handles both cases

/// Run multiple servers until shutdown signal
///
/// Starts multiple server listen tasks and waits for a shutdown signal.
/// When signal is received, stops all servers.
///
/// @tparam Servers Variadic server types
/// @tparam ListenTasks Variadic listen task types
/// @param servers Tuple of server references
/// @param listen_tasks Tuple of listen tasks
/// @param signals Signals to wait for shutdown
///
/// Example:
/// @code
/// coro::task<void> run_servers() {
///     http::server http_srv(router);
///     websocket::ws_server ws_srv(ws_router);
///
///     co_await serve_all(
///         std::tie(http_srv, ws_srv),
///         std::make_tuple(
///             http_srv.listen(addr1),
///             ws_srv.listen(addr2)
///         )
///     );
/// }
/// @endcode
template<typename... Servers, typename... ListenTasks>
coro::task<void> serve_all(std::tuple<Servers&...> servers,
                           std::tuple<ListenTasks...> listen_tasks,
                           std::initializer_list<int> signals = default_shutdown_signals)
{
    // Set up signal handling
    signal::signal_set sigs(signals);
    signal::signal_fd sigfd(sigs);

    // Spawn all listen tasks
    auto spawn_tasks = [](auto&&... tasks) {
        return std::make_tuple(std::move(tasks).spawn()...);
    };
    auto handles = std::apply(spawn_tasks, std::move(listen_tasks));

    // Wait for shutdown signal
    auto info = co_await sigfd.wait();
    if (info) {
        ELIO_LOG_INFO("Received SIG{}, initiating shutdown...", info->name());
    }

    // Stop all servers
    std::apply([](auto&... srvs) { (srvs.stop(), ...); }, servers);

    // Wait for all listen tasks to complete
    auto await_handles = [](auto&&... hs) -> coro::task<void> {
        (co_await hs, ...);
    };
    co_await std::apply(await_handles, std::move(handles));

    ELIO_LOG_INFO("All servers stopped");
    co_return;
}

} // namespace elio
