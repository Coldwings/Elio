#include <elio/net/resolve_wait.hpp>

#include <chrono>
#include <concepts>
#include <exception>
#include <string>
#include <thread>
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
    if (options.domain->outstanding() != 0) return 5;

    elio::runtime::scheduler sched(1);
    sched.start();
    options.deadline.reset();
    options.lookup.use_cache = false;
    options.domain = std::make_shared<elio::net::resolve_domain>(1);
    elio::coro::cancel_source lookup_cancel;
    auto observer = sched.go_joinable(elio::net::resolve_all(
        "localhost", 8080, options, lookup_cancel.get_token()));
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!observer.await_ready() && std::chrono::steady_clock::now() < limit) {
        std::this_thread::yield();
    }
    const bool lookup_expired = !observer.await_ready();
    if (lookup_expired) lookup_cancel.cancel();
    while (!observer.await_ready()) std::this_thread::yield();
    auto resolved = observer.await_resume();
    observer.wait_destroyed();
    sched.shutdown();
    if (lookup_expired || !resolved || resolved.error || resolved.addresses.front().port() != 8080) return 6;
    return options.domain->outstanding() == 0 ? 0 : 7;
}
