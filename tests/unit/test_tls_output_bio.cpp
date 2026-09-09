#include <catch2/catch_test_macros.hpp>

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS && defined(ELIO_RUNTIME_TEST_HOOKS)
#include <elio/tls/detail/output_bio.hpp>

#include <array>
#include <cerrno>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

namespace {
using elio::tls::detail::output_bio_state;
using owned_bio = std::unique_ptr<BIO, decltype(&BIO_free)>;

struct output_script {
    std::vector<ssize_t> results;
    size_t calls = 0;
    const void* last_data = nullptr;
    size_t last_size = 0;
    int last_flags = 0;
    size_t allocations = 0;
    int allocation_failure = 0; // 1 metadata, 2 payload, 3 non-allocation exception.

    static ssize_t send(void* opaque, int, const void* data, size_t size, int flags) {
        auto& script = *static_cast<output_script*>(opaque);
        script.last_data = data;
        script.last_size = size;
        script.last_flags = flags;
        const auto index = script.calls++;
        const auto result = index < script.results.size() ? script.results[index] : static_cast<ssize_t>(size);
        if (result < 0) { errno = static_cast<int>(-result); return -1; }
        return result;
    }
    static void allocate(void* opaque, bool payload) {
        auto& script = *static_cast<output_script*>(opaque);
        ++script.allocations;
        if (script.allocation_failure == 3) throw std::runtime_error("allocation checkpoint");
        if (script.allocation_failure == (payload ? 2 : 1)) throw std::bad_alloc();
    }
    output_bio_state::test_hooks hooks() { return {this, send, allocate}; }
};

std::string queued_text(const output_bio_state& state) {
    const auto view = state.pending();
    return {reinterpret_cast<const char*>(view.data()), view.size()};
}
}

TEST_CASE("TLS output BIO native sends do not own the socket", "[tls][output_bio]") {
    struct sockets {
        std::array<int, 2> fds{-1, -1};
        ~sockets() { for (const auto fd : fds) if (fd >= 0) ::close(fd); }
    } pair;
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                         0, pair.fds.data()) == 0);
    output_bio_state state(pair.fds[0], 8);
    owned_bio bio(state.make_bio(), BIO_free);
    REQUIRE(bio);
    size_t written = 0;
    REQUIRE(BIO_write_ex(bio.get(), "wire", 4, &written) == 1);
    REQUIRE(written == 4);
    std::array<char, 8> received{};
    REQUIRE(::recv(pair.fds[1], received.data(), received.size(), MSG_DONTWAIT) == 4);
    REQUIRE(std::string_view(received.data(), 4) == "wire");
    REQUIRE(state.retained_bytes() == 0);
    REQUIRE(state.drained_bytes() == 4);
    bio.reset();
    REQUIRE(::send(pair.fds[0], "!", 1, MSG_DONTWAIT | MSG_NOSIGNAL) == 1);
}

TEST_CASE("TLS output BIO sends direct ciphertext without queue allocation", "[tls][output_bio]") {
    output_script script;
    output_bio_state state(-1, 0);
    state.set_test_hooks(script.hooks());
    owned_bio bio(state.make_bio(), BIO_free);
    REQUIRE(bio);
    const std::string bytes = "ciphertext";
    REQUIRE(BIO_write(bio.get(), bytes.data(), static_cast<int>(bytes.size())) == static_cast<int>(bytes.size()));
    REQUIRE(script.calls == 1);
    REQUIRE(script.last_data == bytes.data());
    REQUIRE(script.last_size == bytes.size());
    REQUIRE(script.last_flags == (MSG_DONTWAIT | MSG_NOSIGNAL));
    REQUIRE(script.allocations == 0);
    REQUIRE(state.retained_bytes() == 0);
    REQUIRE(state.pending().empty());
    REQUIRE(state.accepted_bytes() == bytes.size());
    REQUIRE(state.drained_bytes() == bytes.size());
    REQUIRE(BIO_flush(bio.get()) == 1);
}

TEST_CASE("TLS output BIO queues unsent suffix after partial or recoverable send", "[tls][output_bio]") {
    for (const ssize_t initial : {ssize_t{2}, ssize_t{-EAGAIN}, ssize_t{-EINTR}}) {
        CAPTURE(initial);
        output_script script{{initial}};
        output_bio_state state(-1, 8);
        state.set_test_hooks(script.hooks());
        owned_bio bio(state.make_bio(), BIO_free);
        REQUIRE(bio);
        std::string bytes = "abcdef";
        REQUIRE(BIO_write(bio.get(), bytes.data(), 6) == 6);
        REQUIRE_FALSE(BIO_should_retry(bio.get()));
        const size_t sent = initial > 0 ? static_cast<size_t>(initial) : 0;
        REQUIRE(queued_text(state) == bytes.substr(sent));
        REQUIRE(state.pending().data() != reinterpret_cast<const std::byte*>(bytes.data() + sent));
        bytes.assign("XXXXXX");
        REQUIRE(queued_text(state) == std::string("abcdef").substr(sent));
        REQUIRE(state.accepted_bytes() == 6);
        REQUIRE(state.drained_bytes() == sent);
        REQUIRE(state.retained_bytes() == 6 - sent);
        state.consume(6 - sent);
        REQUIRE(state.retained_bytes() == 0);
        REQUIRE(state.drained_bytes() == 6);
    }
}

TEST_CASE("TLS output BIO appends behind stable drain leases and consumes blocks", "[tls][output_bio]") {
    output_script script{{-EAGAIN}};
    output_bio_state state(-1, 8);
    state.set_test_hooks(script.hooks());
    owned_bio bio(state.make_bio(), BIO_free);
    REQUIRE(bio);
    REQUIRE(BIO_write(bio.get(), "abcd", 4) == 4);
    const auto lease = state.pending();
    REQUIRE(BIO_write(bio.get(), "efgh", 4) == 4);
    REQUIRE(script.calls == 1);
    REQUIRE(state.pending().data() == lease.data());
    REQUIRE(std::memcmp(lease.data(), "abcd", 4) == 0);
    REQUIRE(state.retained_bytes() == 8);
    REQUIRE(state.pending_bytes() == 8);
    REQUIRE(BIO_wpending(bio.get()) == 8);
    state.consume(3);
    REQUIRE(state.retained_bytes() == 8);
    REQUIRE(queued_text(state) == "d");
    state.consume(3); // Last byte of first block and first two of second.
    REQUIRE(state.retained_bytes() == 4);
    REQUIRE(state.pending_bytes() == 2);
    REQUIRE(queued_text(state) == "gh");
    state.consume(2);
    REQUIRE(state.retained_bytes() == 0);
    REQUIRE(state.drained_bytes() == 8);
    REQUIRE(BIO_write(bio.get(), "i", 1) == 1);
    REQUIRE(script.calls == 2);
    REQUIRE(state.accepted_bytes() == 9);
    REQUIRE(state.drained_bytes() == 9);
}

TEST_CASE("TLS output BIO budget includes consumed prefixes and latches exhaustion", "[tls][output_bio]") {
    output_script script{{-EAGAIN}};
    output_bio_state state(-1, 4);
    state.set_test_hooks(script.hooks());
    owned_bio bio(state.make_bio(), BIO_free);
    REQUIRE(bio);
    REQUIRE(BIO_write(bio.get(), "abcd", 4) == 4);
    state.consume(3);
    const auto lease = state.pending();
    REQUIRE(state.pending_bytes() == 1);
    REQUIRE(state.retained_bytes() == 4);
    REQUIRE(BIO_write(bio.get(), "e", 1) == -1);
    REQUIRE(state.error() == ENOBUFS);
    REQUIRE_FALSE(BIO_should_retry(bio.get()));
    REQUIRE(state.pending().empty());
    REQUIRE(*lease.data() == std::byte{'d'});
    state.consume(1); // Already leased I/O can still retire after failure.
    REQUIRE(state.retained_bytes() == 0);
    REQUIRE(state.accepted_bytes() == 4);
    REQUIRE(state.drained_bytes() == 4);
    REQUIRE(BIO_write(bio.get(), "f", 1) == -1);
    REQUIRE(script.calls == 1);
    REQUIRE(BIO_flush(bio.get()) == 0);
    state.fail(EIO);
    REQUIRE(state.error() == ENOBUFS);
}

TEST_CASE("TLS output BIO records directly sent prefix even when suffix cannot fit", "[tls][output_bio]") {
    output_script script{{2}};
    output_bio_state state(-1, 1);
    state.set_test_hooks(script.hooks());
    owned_bio bio(state.make_bio(), BIO_free);
    REQUIRE(bio);
    REQUIRE(BIO_write(bio.get(), "abcd", 4) == -1);
    REQUIRE(state.error() == ENOBUFS);
    REQUIRE(state.accepted_bytes() == 2);
    REQUIRE(state.drained_bytes() == 2);
    REQUIRE(state.retained_bytes() == 0);
    REQUIRE(script.allocations == 0);
    REQUIRE(BIO_write(bio.get(), "later", 5) == -1);
    REQUIRE(script.calls == 1);
}

TEST_CASE("TLS output BIO callbacks contain allocation failures", "[tls][output_bio]") {
    for (const int mode : {1, 2, 3}) {
        CAPTURE(mode);
        output_script script{{2}};
        script.allocation_failure = mode;
        output_bio_state state(-1, 8);
        state.set_test_hooks(script.hooks());
        owned_bio bio(state.make_bio(), BIO_free);
        REQUIRE(bio);
        int result = 0;
        REQUIRE_NOTHROW(result = BIO_write(bio.get(), "abcd", 4));
        REQUIRE(result == -1);
        REQUIRE(state.error() == (mode == 3 ? EIO : ENOMEM));
        REQUIRE_FALSE(BIO_should_retry(bio.get()));
        REQUIRE(state.accepted_bytes() == 2);
        REQUIRE(state.drained_bytes() == 2);
        REQUIRE(state.retained_bytes() == 0);
        REQUIRE(BIO_write(bio.get(), "later", 5) == -1);
        REQUIRE(script.calls == 1);
    }
}

TEST_CASE("TLS output BIO transport failures prevent all later output", "[tls][output_bio]") {
    for (const ssize_t initial : {ssize_t{-EPIPE}, ssize_t{-ECONNRESET}, ssize_t{0}}) {
        CAPTURE(initial);
        output_script script{{initial}};
        output_bio_state state(-1, 8);
        state.set_test_hooks(script.hooks());
        owned_bio bio(state.make_bio(), BIO_free);
        REQUIRE(bio);
        REQUIRE(BIO_write(bio.get(), "data", 4) == -1);
        REQUIRE(state.error() == (initial == -ECONNRESET ? ECONNRESET : EPIPE));
        REQUIRE(state.accepted_bytes() == 0);
        REQUIRE(state.drained_bytes() == 0);
        REQUIRE(script.allocations == 0);
        REQUIRE(BIO_write(bio.get(), "later", 5) == -1);
        REQUIRE(script.calls == 1);
    }
}
#endif
