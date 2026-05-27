// Stage S6 — memory_region<Backend> RAII wrapper.
//
// Each test verifies a single contract:
//   * Construction calls Backend::register_mr with the user's args
//     verbatim and stores the returned MR handle.
//   * Destruction calls Backend::dereg_mr exactly once on that handle.
//   * view() / view(off, len) returns a buffer_view with the right
//     addr / length / lkey.
//   * remote() / remote(off, len) returns a remote_buffer with the
//     right addr / length / rkey.
//   * Move-construct and move-assign transfer ownership; the
//     moved-from object does NOT call dereg_mr on the moved handle.
//   * The polymorphic specialisation routes register_mr / dereg_mr /
//     lkey_of / rkey_of through the vtable.

#include <catch2/catch_test_macros.hpp>

#include <elio/rdma/rdma.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <utility>

using elio::rdma::buffer_view;
using elio::rdma::memory_region;
using elio::rdma::polymorphic_backend;
using elio::rdma::remote_buffer;
using elio::rdma::sge;
using elio::rdma::send_flags;
using elio::rdma::wr_id;

namespace {

struct mr_state {
    std::atomic<int> registers{0};
    std::atomic<int> deregs{0};
    void*            last_pd = nullptr;
    void*            last_addr = nullptr;
    std::size_t      last_length = 0;
    int              last_access = 0;
    void*            last_dereg_mr = nullptr;

    // Fake handle returned by register_mr. The tests pass a unique
    // handle value so we can assert it round-trips.
    void*            handle_to_return = nullptr;
    std::uint32_t    lkey_to_return   = 0;
    std::uint32_t    rkey_to_return   = 0;
};

struct mr_static_backend {
    static inline mr_state* state = nullptr;

    // backend_traits surface (unused by these tests but required to
    // satisfy backend_with_mr).
    static int post_send(void*, std::span<const sge>,
                         send_flags, wr_id) noexcept { return 0; }
    static int post_recv(void*, std::span<const sge>, wr_id) noexcept {
        return 0;
    }
    static int post_rdma_write(void*, std::span<const sge>,
                               remote_buffer, send_flags, wr_id) noexcept {
        return 0;
    }
    static int post_rdma_read(void*, std::span<const sge>,
                              remote_buffer, wr_id) noexcept {
        return 0;
    }

    // MR surface.
    static void* register_mr(void* pd, void* addr, std::size_t length,
                             int access) noexcept {
        if (!state) return nullptr;
        state->registers.fetch_add(1);
        state->last_pd     = pd;
        state->last_addr   = addr;
        state->last_length = length;
        state->last_access = access;
        return state->handle_to_return;
    }
    static void dereg_mr(void* mr) noexcept {
        if (!state) return;
        state->deregs.fetch_add(1);
        state->last_dereg_mr = mr;
    }
    static std::uint32_t lkey_of(void*) noexcept {
        return state ? state->lkey_to_return : 0;
    }
    static std::uint32_t rkey_of(void*) noexcept {
        return state ? state->rkey_to_return : 0;
    }
};

struct state_guard {
    explicit state_guard(mr_state* s) noexcept {
        mr_static_backend::state = s;
    }
    ~state_guard() noexcept { mr_static_backend::state = nullptr; }
    state_guard(const state_guard&) = delete;
    state_guard& operator=(const state_guard&) = delete;
};

struct mr_poly_backend : polymorphic_backend {
    mr_state state;

    int post_send(void*, std::span<const sge>,
                  send_flags, wr_id) noexcept override { return 0; }
    int post_recv(void*, std::span<const sge>, wr_id) noexcept override {
        return 0;
    }
    int post_rdma_write(void*, std::span<const sge>,
                        remote_buffer, send_flags, wr_id) noexcept override {
        return 0;
    }
    int post_rdma_read(void*, std::span<const sge>,
                       remote_buffer, wr_id) noexcept override {
        return 0;
    }

    void* register_mr(void* pd, void* addr, std::size_t length,
                      int access) noexcept override {
        state.registers.fetch_add(1);
        state.last_pd     = pd;
        state.last_addr   = addr;
        state.last_length = length;
        state.last_access = access;
        return state.handle_to_return;
    }
    void dereg_mr(void* mr) noexcept override {
        state.deregs.fetch_add(1);
        state.last_dereg_mr = mr;
    }
    std::uint32_t lkey_of(void*) noexcept override {
        return state.lkey_to_return;
    }
    std::uint32_t rkey_of(void*) noexcept override {
        return state.rkey_to_return;
    }
};

constexpr auto fake_handle(std::uintptr_t v) noexcept {
    return reinterpret_cast<void*>(v);
}

}  // namespace

TEST_CASE("memory_region<Backend>: register on construct, deregister on destroy",
          "[rdma][mr]") {
    mr_state st;
    st.handle_to_return = fake_handle(0xDEAD);
    st.lkey_to_return   = 0x1111;
    st.rkey_to_return   = 0x2222;
    state_guard guard{&st};

    alignas(8) char payload[1024];
    void* pd = fake_handle(0xCAFE);

    {
        memory_region<mr_static_backend> mr{pd, payload,
                                            sizeof(payload), /*access=*/0x7};
        REQUIRE(mr.ok());
        REQUIRE(mr.native() == fake_handle(0xDEAD));
        REQUIRE(mr.addr() == payload);
        REQUIRE(mr.length() == sizeof(payload));
        REQUIRE(mr.lkey() == 0x1111u);
        REQUIRE(mr.rkey() == 0x2222u);

        REQUIRE(st.registers.load() == 1);
        REQUIRE(st.last_pd == pd);
        REQUIRE(st.last_addr == payload);
        REQUIRE(st.last_length == sizeof(payload));
        REQUIRE(st.last_access == 0x7);
        REQUIRE(st.deregs.load() == 0);
    }

    REQUIRE(st.deregs.load() == 1);
    REQUIRE(st.last_dereg_mr == fake_handle(0xDEAD));
}

TEST_CASE("memory_region<Backend>::view returns buffer_view with lkey",
          "[rdma][mr][view]") {
    mr_state st;
    st.handle_to_return = fake_handle(1);
    st.lkey_to_return   = 0xFEED;
    state_guard guard{&st};

    alignas(8) char payload[256] = {};
    memory_region<mr_static_backend> mr{nullptr, payload, sizeof(payload), 0};

    buffer_view full = mr.view();
    REQUIRE(full.addr == payload);
    REQUIRE(full.length == sizeof(payload));
    REQUIRE(full.lkey == 0xFEEDu);

    buffer_view slice = mr.view(64, 32);
    REQUIRE(slice.addr == payload + 64);
    REQUIRE(slice.length == 32u);
    REQUIRE(slice.lkey == 0xFEEDu);
}

TEST_CASE("memory_region<Backend>::remote returns remote_buffer with rkey",
          "[rdma][mr][remote]") {
    mr_state st;
    st.handle_to_return = fake_handle(2);
    st.rkey_to_return   = 0xBABE;
    state_guard guard{&st};

    alignas(8) char payload[256] = {};
    memory_region<mr_static_backend> mr{nullptr, payload, sizeof(payload), 0};

    remote_buffer full = mr.remote();
    REQUIRE(full.addr == reinterpret_cast<std::uint64_t>(payload));
    REQUIRE(full.length == 256u);
    REQUIRE(full.rkey == 0xBABEu);

    remote_buffer slice = mr.remote(128, 64);
    REQUIRE(slice.addr == reinterpret_cast<std::uint64_t>(payload) + 128);
    REQUIRE(slice.length == 64u);
    REQUIRE(slice.rkey == 0xBABEu);
}

TEST_CASE("memory_region<Backend>: move-construct transfers ownership "
          "without double-deregister",
          "[rdma][mr][move]") {
    mr_state st;
    st.handle_to_return = fake_handle(0xABCD);
    state_guard guard{&st};

    alignas(8) char payload[64] = {};
    {
        memory_region<mr_static_backend> a{nullptr, payload,
                                           sizeof(payload), 0};
        REQUIRE(st.registers.load() == 1);
        memory_region<mr_static_backend> b{std::move(a)};
        // a is now empty; b owns the handle. No new register_mr.
        REQUIRE(st.registers.load() == 1);
        REQUIRE_FALSE(a.ok());
        REQUIRE(b.ok());
        REQUIRE(b.native() == fake_handle(0xABCD));
    }
    // Only b's destructor calls dereg_mr.
    REQUIRE(st.deregs.load() == 1);
}

TEST_CASE("memory_region<Backend>: move-assign deregisters previous handle",
          "[rdma][mr][move]") {
    mr_state st;
    state_guard guard{&st};

    alignas(8) char buf1[32] = {};
    alignas(8) char buf2[32] = {};

    st.handle_to_return = fake_handle(0x1);
    memory_region<mr_static_backend> a{nullptr, buf1, sizeof(buf1), 0};

    st.handle_to_return = fake_handle(0x2);
    memory_region<mr_static_backend> b{nullptr, buf2, sizeof(buf2), 0};

    REQUIRE(st.registers.load() == 2);
    REQUIRE(st.deregs.load() == 0);

    a = std::move(b);
    // a's previous handle (0x1) deregistered; a now owns 0x2.
    REQUIRE(st.deregs.load() == 1);
    REQUIRE(st.last_dereg_mr == fake_handle(0x1));
    REQUIRE(a.native() == fake_handle(0x2));
    REQUIRE_FALSE(b.ok());
}

TEST_CASE("memory_region<polymorphic_backend>: routes through the vtable",
          "[rdma][mr][polymorphic]") {
    mr_poly_backend impl{};
    impl.state.handle_to_return = fake_handle(0xDEED);
    impl.state.lkey_to_return   = 0xCC;
    impl.state.rkey_to_return   = 0xDD;

    alignas(8) char payload[128] = {};
    void* pd = fake_handle(0x10);

    {
        memory_region<polymorphic_backend> mr{impl, pd, payload,
                                              sizeof(payload), 0xF};
        REQUIRE(mr.ok());
        REQUIRE(mr.native() == fake_handle(0xDEED));
        REQUIRE(mr.lkey() == 0xCCu);
        REQUIRE(mr.rkey() == 0xDDu);

        REQUIRE(impl.state.registers.load() == 1);
        REQUIRE(impl.state.last_pd == pd);
        REQUIRE(impl.state.last_access == 0xF);
    }

    REQUIRE(impl.state.deregs.load() == 1);
    REQUIRE(impl.state.last_dereg_mr == fake_handle(0xDEED));
}

TEST_CASE("memory_region<polymorphic_backend>: subclass without "
          "register_mr override yields !ok() and no dereg",
          "[rdma][mr][polymorphic][default]") {
    struct no_mr_backend : polymorphic_backend {
        int post_send(void*, std::span<const sge>,
                      send_flags, wr_id) noexcept override { return 0; }
        int post_recv(void*, std::span<const sge>, wr_id) noexcept override {
            return 0;
        }
        int post_rdma_write(void*, std::span<const sge>,
                            remote_buffer, send_flags, wr_id) noexcept override {
            return 0;
        }
        int post_rdma_read(void*, std::span<const sge>,
                           remote_buffer, wr_id) noexcept override {
            return 0;
        }
    };

    no_mr_backend impl{};
    char buf[8] = {};
    memory_region<polymorphic_backend> mr{impl, nullptr, buf,
                                          sizeof(buf), 0};
    REQUIRE_FALSE(mr.ok());
    REQUIRE(mr.native() == nullptr);
    REQUIRE(mr.lkey() == 0u);
    REQUIRE(mr.rkey() == 0u);
    // Destructor runs at scope exit; must NOT call dereg_mr on a null
    // handle. ASAN would catch a misuse here.
}
