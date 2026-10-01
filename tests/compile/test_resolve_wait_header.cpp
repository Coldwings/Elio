#include <elio/net/resolve_wait.hpp>

#include <chrono>
#include <concepts>
#include <exception>
#include <string>
#include <utility>

#ifdef ELIO_RUNTIME_TEST_HOOKS
#error Direct-header consumer must use the production resolver path
#endif

static_assert(std::same_as<decltype(elio::net::resolve_all("localhost", 80, {})),
    elio::coro::task<std::vector<elio::net::socket_address>>>);
static_assert(std::same_as<decltype(elio::net::resolve_hostname("localhost", 80, {})),
    elio::coro::task<std::optional<elio::net::socket_address>>>);
static_assert(std::same_as<decltype(elio::net::resolve_all("localhost", 80,
    elio::net::resolve_wait_options{}, elio::coro::cancel_token{})),
    elio::coro::task<elio::net::resolve_result>>);

template<typename T>
T run_immediate(elio::coro::task<T> operation) {
    auto handle = elio::coro::detail::task_access::handle(operation);
    handle.resume();
    if (!handle.done()) std::terminate();
    return operation.await_resume();
}

int main() {
    elio::net::resolve_wait_options options;
    options.domain = std::make_shared<elio::net::resolve_domain>(0);
    auto literal = run_immediate(elio::net::resolve_all("127.0.0.1", 8080, options, {}));
    if (!literal || literal.error || literal.addresses.front().port() != 8080) return 1;
    auto pending = run_immediate(elio::net::resolve_all("requires-worker.example", 80, options, {}));
    if (pending || pending.error != ENOTSUP) return 2;
    elio::coro::cancel_source source;
    source.cancel();
    auto cancelled = run_immediate(elio::net::resolve_all("127.0.0.1", 80, options, source.get_token()));
    if (cancelled.status != elio::net::resolve_status::cancelled || cancelled.error != ECANCELED) return 3;
    options.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    auto expired = run_immediate(elio::net::resolve_all("127.0.0.1", 80, options, {}));
    if (expired.status != elio::net::resolve_status::timed_out || expired.error != ETIMEDOUT) return 4;
    return options.domain->outstanding() == 0 ? 0 : 5;
}
