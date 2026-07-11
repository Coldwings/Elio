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
///     http::server srv(std::move(r));
///     co_await elio::serve(srv, [&]() {
///         return srv.listen(net::ipv4_address(8080));
///     });
///
///     co_return 0;
/// }
///
/// int main(int argc, char* argv[]) {
///     elio::signal::signal_set shutdown_signals(elio::default_shutdown_signals);
///     shutdown_signals.block_all_threads();
///     return elio::run(async_main, argc, argv);
/// }
/// @endcode

#include <elio/coro/task.hpp>
#include <elio/signal/signalfd.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>
#include <elio/log/macros.hpp>

#include <chrono>
#include <csignal>
#include <exception>
#include <initializer_list>
#include <mutex>
#include <type_traits>

namespace elio {

/// Default shutdown signals
inline constexpr std::initializer_list<int> default_shutdown_signals = {SIGINT, SIGTERM};

namespace detail {

/// Install ``SIG_IGN`` for ``SIGPIPE`` exactly once per process. See the
/// matching helper in ``runtime/async_main.hpp`` for the full rationale —
/// this duplicate exists so users that wire ``serve()`` into a custom
/// scheduler (without going through ``elio::run`` / ``ELIO_ASYNC_MAIN``)
/// still get the protection. ``sigaction`` is idempotent, so running the
/// install twice is harmless.
inline void ignore_sigpipe_in_serve_once() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        struct sigaction sa{};
        sa.sa_handler = SIG_IGN;
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        ::sigaction(SIGPIPE, &sa, nullptr);
    });
}

template<typename Server>
coro::task<void> wait_active_connections_drained(Server& server) {
    if constexpr (requires { server.active_connections(); }) {
        while (server.active_connections() != 0) {
            co_await time::sleep_for(std::chrono::milliseconds(10));
        }
    }
    co_return;
}

} // namespace detail

/// Wait for shutdown signals (SIGINT or SIGTERM)
///
/// This is a simple awaitable that blocks until a shutdown signal is received.
/// Useful for implementing custom server loops.
///
/// @param signals Signal set to wait for (defaults to SIGINT, SIGTERM)
/// @return signal_info about the received signal
///
/// @note Process-directed shutdown signals must be blocked before scheduler
/// threads are created, so they remain pending for signalfd instead of being
/// delivered to an unmasked thread. Use an explicit main() that calls
/// signal_set::block_all_threads() before elio::run().
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
/// @tparam ListenFunc Callable that returns a listen coroutine task
/// @param server Reference to the server (used to call stop())
/// @param listen_func Function that returns the listen coroutine task
/// @param signals Signals to wait for shutdown (defaults to SIGINT, SIGTERM)
///
/// @note Block the same shutdown signals before starting the scheduler, for
/// example in main() before elio::run(). ELIO_ASYNC_MAIN does not mask the
/// calling thread, so a process-directed SIGINT/SIGTERM can terminate the
/// process before serve() observes it.
///
/// Example:
/// @code
/// coro::task<int> async_main(int argc, char* argv[]) {
///     http::router r;
///     r.get("/", handler);
///
///     http::server srv(std::move(r));
///     co_await serve(srv, [&]() { return srv.listen(net::ipv4_address(8080)); });
///
///     co_return 0;
/// }
///
/// int main(int argc, char* argv[]) {
///     elio::signal::signal_set shutdown_signals(elio::default_shutdown_signals);
///     shutdown_signals.block_all_threads();
///     return elio::run(async_main, argc, argv);
/// }
/// @endcode
template<typename Server, typename ListenFunc>
    requires std::invocable<ListenFunc>
coro::task<void> serve(Server& server, ListenFunc listen_func,
                       std::initializer_list<int> signals = default_shutdown_signals)
{
    // Mask SIGPIPE so writes on half-closed sockets surface as EPIPE
    // io_results rather than terminating the process.
    detail::ignore_sigpipe_in_serve_once();

    // Set up signal handling
    signal::signal_set sigs(signals);
    signal::signal_fd sigfd(sigs);

    // Get the scheduler
    auto* sched = runtime::scheduler::current();
    if (!sched) {
        ELIO_LOG_ERROR("serve() must be called within a scheduler context");
        co_return;
    }

    // Spawn the listen task as a joinable coroutine
    auto listen_handle = sched->go_joinable(std::move(listen_func));

    // Wait for shutdown signal
    auto info = co_await sigfd.wait();
    if (info) {
        ELIO_LOG_INFO("Received SIG{}, initiating shutdown...", info->name());
    }

    // Stop the server
    server.stop();

    // Wait for listen task to complete, then drain any detached connection
    // handlers tracked by the server before a stack server can be destroyed.
    std::exception_ptr listen_error;
    try {
        co_await listen_handle;
    } catch (...) {
        listen_error = std::current_exception();
    }

    co_await detail::wait_active_connections_drained(server);

    if (listen_error) {
        std::rethrow_exception(listen_error);
    }

    ELIO_LOG_INFO("Server stopped");
    co_return;
}

/// Serve a server with TLS until shutdown signal is received
///
/// Convenience overload for TLS servers.
///
/// @tparam Server Server type (must have stop() method)
/// @tparam ListenFunc Callable type returning the server's TLS listen task
/// @param server Reference to the server
/// @param listen_func Callable returning the TLS listen coroutine task
/// @param signals Signals to wait for shutdown
///
/// @note See serve() for the required shutdown-signal masking setup.
///
/// Example:
/// @code
/// coro::task<int> async_main(int argc, char* argv[]) {
///     http::router r;
///     r.get("/", handler);
///
///     http::server srv(std::move(r));
///     auto tls_ctx = tls::tls_context::make_server("cert.pem", "key.pem");
///     auto addr = net::ipv4_address(8443);
///
///     co_await serve(srv, [&]() { return srv.listen_tls(addr, tls_ctx); });
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
/// @tparam ListenFuncs Variadic listen function types
/// @param servers Tuple of server references
/// @param listen_funcs Tuple of listen functions (each returning a task)
/// @param signals Signals to wait for shutdown
///
/// Example:
/// @code
/// coro::task<void> run_servers() {
///     http::server http_srv(std::move(http_router));
///     websocket::ws_server ws_srv(std::move(ws_router));
///
///     co_await serve_all(
///         std::tie(http_srv, ws_srv),
///         std::make_tuple(
///             [&]() { return http_srv.listen(addr1); },
///             [&]() { return ws_srv.listen(addr2); }
///         )
///     );
/// }
///
/// int main() {
///     elio::signal::signal_set shutdown_signals(elio::default_shutdown_signals);
///     shutdown_signals.block_all_threads();
///     return elio::run(run_servers);
/// }
/// @endcode
template<typename... Servers, typename... ListenFuncs>
coro::task<void> serve_all(std::tuple<Servers&...> servers,
                           std::tuple<ListenFuncs...> listen_funcs,
                           std::initializer_list<int> signals = default_shutdown_signals)
{
    // Mask SIGPIPE so writes on half-closed sockets surface as EPIPE
    // io_results rather than terminating the process.
    detail::ignore_sigpipe_in_serve_once();

    // Set up signal handling
    signal::signal_set sigs(signals);
    signal::signal_fd sigfd(sigs);

    // Get the scheduler
    auto* sched = runtime::scheduler::current();
    if (!sched) {
        ELIO_LOG_ERROR("serve_all() must be called within a scheduler context");
        co_return;
    }

    // Spawn all listen tasks as joinable coroutines
    auto spawn_tasks = [sched](auto&&... funcs) {
        return std::make_tuple(sched->go_joinable(std::move(funcs))...);
    };
    auto handles = std::apply(spawn_tasks, std::move(listen_funcs));

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
    std::exception_ptr listen_error;
    try {
        co_await std::apply(await_handles, std::move(handles));
    } catch (...) {
        listen_error = std::current_exception();
    }

    auto await_drains = [](auto&... srvs) -> coro::task<void> {
        (co_await detail::wait_active_connections_drained(srvs), ...);
        co_return;
    };
    co_await std::apply(await_drains, servers);

    if (listen_error) {
        std::rethrow_exception(listen_error);
    }

    ELIO_LOG_INFO("All servers stopped");
    co_return;
}

} // namespace elio
