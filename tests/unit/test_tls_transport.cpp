#include <catch2/catch_test_macros.hpp>

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS && defined(ELIO_RUNTIME_TEST_HOOKS)
#include <elio/tls/detail/tls_transport.hpp>

namespace {
using elio::tls::detail::tls_transport;
using elio::coro::detail::task_access;

std::shared_ptr<tls_transport> notification_transport() {
    return std::make_shared<tls_transport>(elio::net::tcp_stream{-1}, 32);
}
}

TEST_CASE("TLS progress before waiter publication is retained", "[tls][transport][issue-1215]") {
    auto transport = notification_transport();
    auto wait = transport->wait_change(transport->generation, {});
    transport->notify_progress();
    auto handle = task_access::handle(wait);
    handle.resume();
    REQUIRE(handle.done());
    CHECK(wait.await_resume().result == 0);
}

TEST_CASE("TLS progress wakes all published waiters exactly once", "[tls][transport][issue-1215]") {
    auto transport = notification_transport();
    auto first = transport->wait_change(0, {});
    auto second = transport->wait_change(0, {});
    auto first_handle = task_access::handle(first);
    auto second_handle = task_access::handle(second);
    first_handle.resume();
    second_handle.resume();
    const bool both_parked = !first_handle.done() && !second_handle.done();
    transport->notify_progress();
    transport->notify_progress();
    REQUIRE(first_handle.done());
    REQUIRE(second_handle.done());
    CHECK(both_parked);
    CHECK(first.await_resume().result == 0);
    CHECK(second.await_resume().result == 0);
}

TEST_CASE("TLS cancelled progress waiter does not consume sibling notification", "[tls][transport][issue-1215]") {
    auto transport = notification_transport();
    elio::coro::cancel_source cancel;
    auto first = transport->wait_change(0, cancel.get_token());
    auto second = transport->wait_change(0, {});
    auto first_handle = task_access::handle(first);
    auto second_handle = task_access::handle(second);
    first_handle.resume();
    second_handle.resume();
    cancel.cancel();
    const bool second_parked = !second_handle.done();
    transport->notify_progress();
    REQUIRE(first_handle.done());
    REQUIRE(second_handle.done());
    CHECK(second_parked);
    CHECK(first.await_resume().result == -ECANCELED);
    CHECK(second.await_resume().result == 0);
}

TEST_CASE("TLS read poll rechecks progress at source publication", "[tls][transport][issue-1215]") {
    auto transport = notification_transport();
    transport->read_publish_context = transport.get();
    transport->before_read_publish = [](void* context) {
        static_cast<tls_transport*>(context)->notify_progress();
    };
    auto wait = transport->wait_read(0, {});
    auto handle = task_access::handle(wait);
    // No scheduler or socket is available: success must come from the epoch
    // recheck, never a backend poll after the notification was already lost.
    handle.resume();
    REQUIRE(handle.done());
    CHECK(wait.await_resume().result == 0);
    CHECK(transport->generation == 1);
}

TEST_CASE("TLS output launch failure releases pump ownership and wakes waiters", "[tls][transport][issue-1215]") {
    auto transport = notification_transport();
    transport->output.set_test_hooks({nullptr,
        [](void*, int, const void*, size_t, int) -> ssize_t { errno = EAGAIN; return -1; }, nullptr});
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(transport->output.make_bio(), BIO_free);
    REQUIRE(bio);
    REQUIRE(BIO_write(bio.get(), "ciphertext", 10) == 10);
    auto wait = transport->wait_change(0, {});
    auto handle = task_access::handle(wait);
    handle.resume();
    const bool parked = !handle.done();
    // No scheduler is installed on this thread: failure cannot strand an
    // admitted queue or a waiter behind a pump that will never start.
    transport->start_output();
    REQUIRE(handle.done());
    CHECK(parked);
    CHECK(wait.await_resume().result == 0);
    CHECK(transport->output.error() != 0);
    CHECK_FALSE(transport->output_active_for_test());
    auto settled = transport->settle_output();
    auto settled_handle = task_access::handle(settled);
    settled_handle.resume();
    REQUIRE(settled_handle.done());
    settled.await_resume();
}
#endif
