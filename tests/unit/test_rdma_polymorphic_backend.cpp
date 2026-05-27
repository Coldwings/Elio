// Stage S2 — backend_traits concept + polymorphic_backend tests.
//
// Verifies (1) a minimal type satisfies the static traits concept,
// (2) the polymorphic_backend virtual interface dispatches correctly,
// (3) connection<Backend> + connection<polymorphic_backend> can be
// constructed and expose the QP / dispatcher / config they were given,
// (4) the polymorphic_traits_adapter forwards to the underlying virtual
// implementation.

#include <catch2/catch_test_macros.hpp>

#include <elio/rdma/rdma.hpp>

#include <atomic>
#include <cstring>
#include <span>

using elio::rdma::backend_traits;
using elio::rdma::backend_with_mr;
using elio::rdma::backend_with_srq;
using elio::rdma::buffer_view;
using elio::rdma::connection;
using elio::rdma::connection_config;
using elio::rdma::dispatcher;
using elio::rdma::polymorphic_backend;
using elio::rdma::polymorphic_traits_adapter;
using elio::rdma::remote_buffer;
using elio::rdma::send_flags;
using elio::rdma::sge;
using elio::rdma::wr_id;

namespace {

// Counts the calls a backend has received so tests can verify dispatch.
struct call_counts {
    std::atomic<int> sends{0};
    std::atomic<int> recvs{0};
    std::atomic<int> writes{0};
    std::atomic<int> reads{0};
    std::atomic<int> srq_recvs{0};
};

// ---------------------------------------------------------------------
// Static-traits backend (S2 contract check).
//
// All hooks return 0 and bump the corresponding counter. The backend
// holds no state per instance — the counter pointer is passed in via
// a thread-local, since the concept requires *static* members. (A real
// backend would derive the qp/cq pointers from the function-call
// arguments.)
// ---------------------------------------------------------------------
struct static_test_backend {
    static inline call_counts* counts = nullptr;

    static int post_send(void*, std::span<const sge>,
                         send_flags, wr_id) noexcept {
        if (counts) counts->sends.fetch_add(1);
        return 0;
    }
    static int post_recv(void*, std::span<const sge>,
                         wr_id) noexcept {
        if (counts) counts->recvs.fetch_add(1);
        return 0;
    }
    static int post_rdma_write(void*, std::span<const sge>,
                               remote_buffer, send_flags, wr_id) noexcept {
        if (counts) counts->writes.fetch_add(1);
        return 0;
    }
    static int post_rdma_read(void*, std::span<const sge>,
                              remote_buffer, wr_id) noexcept {
        if (counts) counts->reads.fetch_add(1);
        return 0;
    }
};

// Same surface plus the MR extension hooks.
struct static_test_backend_with_mr {
    static int post_send(void*, std::span<const sge>,
                         send_flags, wr_id) noexcept { return 0; }
    static int post_recv(void*, std::span<const sge>, wr_id) noexcept { return 0; }
    static int post_rdma_write(void*, std::span<const sge>,
                               remote_buffer, send_flags, wr_id) noexcept { return 0; }
    static int post_rdma_read(void*, std::span<const sge>,
                              remote_buffer, wr_id) noexcept { return 0; }
    static void* register_mr(void*, void*, std::size_t, int) noexcept {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEAD));
    }
    static void dereg_mr(void*) noexcept {}
};

// A type missing post_recv: must NOT satisfy backend_traits.
struct broken_backend {
    static int post_send(void*, std::span<const sge>,
                         send_flags, wr_id) noexcept { return 0; }
    static int post_rdma_write(void*, std::span<const sge>,
                               remote_buffer, send_flags, wr_id) noexcept { return 0; }
    static int post_rdma_read(void*, std::span<const sge>,
                              remote_buffer, wr_id) noexcept { return 0; }
    // post_recv intentionally omitted.
};

// ---------------------------------------------------------------------
// Polymorphic backend implementation.
// ---------------------------------------------------------------------
struct mock_polymorphic : polymorphic_backend {
    call_counts counts;

    int post_send(void*, std::span<const sge>,
                  send_flags, wr_id) noexcept override {
        counts.sends.fetch_add(1);
        return 0;
    }
    int post_recv(void*, std::span<const sge>, wr_id) noexcept override {
        counts.recvs.fetch_add(1);
        return 0;
    }
    int post_rdma_write(void*, std::span<const sge>,
                        remote_buffer, send_flags, wr_id) noexcept override {
        counts.writes.fetch_add(1);
        return 0;
    }
    int post_rdma_read(void*, std::span<const sge>,
                       remote_buffer, wr_id) noexcept override {
        counts.reads.fetch_add(1);
        return 0;
    }
    int post_srq_recv(void*, std::span<const sge>, wr_id) noexcept override {
        counts.srq_recvs.fetch_add(1);
        return 0;
    }
};

}  // namespace

TEST_CASE("backend_traits concept matches a conforming static backend",
          "[rdma][backend]") {
    STATIC_REQUIRE(backend_traits<static_test_backend>);
    STATIC_REQUIRE(backend_traits<static_test_backend_with_mr>);
    STATIC_REQUIRE_FALSE(backend_traits<broken_backend>);
}

TEST_CASE("backend_with_mr is detected only when register/dereg present",
          "[rdma][backend]") {
    STATIC_REQUIRE(backend_with_mr<static_test_backend_with_mr>);
    STATIC_REQUIRE_FALSE(backend_with_mr<static_test_backend>);
}

TEST_CASE("polymorphic_backend dispatches through the vtable",
          "[rdma][backend][polymorphic]") {
    mock_polymorphic impl{};
    polymorphic_backend& base = impl;

    sge s{nullptr, 0, 0};
    remote_buffer rb{};
    auto sges = std::span<const sge>(&s, 1);

    REQUIRE(base.post_send(nullptr, sges, {}, 0) == 0);
    REQUIRE(base.post_recv(nullptr, sges, 0) == 0);
    REQUIRE(base.post_rdma_write(nullptr, sges, rb, {}, 0) == 0);
    REQUIRE(base.post_rdma_read(nullptr, sges, rb, 0) == 0);
    REQUIRE(base.post_srq_recv(nullptr, sges, 0) == 0);

    REQUIRE(impl.counts.sends.load() == 1);
    REQUIRE(impl.counts.recvs.load() == 1);
    REQUIRE(impl.counts.writes.load() == 1);
    REQUIRE(impl.counts.reads.load() == 1);
    REQUIRE(impl.counts.srq_recvs.load() == 1);
}

TEST_CASE("polymorphic_backend default optional methods report not-supported",
          "[rdma][backend][polymorphic]") {
    struct minimal_poly : polymorphic_backend {
        int post_send(void*, std::span<const sge>, send_flags, wr_id) noexcept override { return 0; }
        int post_recv(void*, std::span<const sge>, wr_id) noexcept override { return 0; }
        int post_rdma_write(void*, std::span<const sge>, remote_buffer, send_flags, wr_id) noexcept override { return 0; }
        int post_rdma_read(void*, std::span<const sge>, remote_buffer, wr_id) noexcept override { return 0; }
    };

    minimal_poly impl{};
    polymorphic_backend& base = impl;

    // Default register_mr returns nullptr (not-supported sentinel),
    // dereg_mr is a no-op, post_srq_recv returns -ENOTSUP (-95).
    REQUIRE(base.register_mr(nullptr, nullptr, 0, 0) == nullptr);
    base.dereg_mr(nullptr);  // must not crash
    sge s{};
    REQUIRE(base.post_srq_recv(nullptr, std::span<const sge>(&s, 1), 0) == -95);
}

TEST_CASE("polymorphic_traits_adapter forwards to the vtable",
          "[rdma][backend][polymorphic]") {
    mock_polymorphic impl{};
    polymorphic_traits_adapter adapter{&impl};

    sge s{};
    remote_buffer rb{};
    auto sges = std::span<const sge>(&s, 1);

    REQUIRE(adapter.post_send(nullptr, sges, {}, 0) == 0);
    REQUIRE(adapter.post_recv(nullptr, sges, 0) == 0);
    REQUIRE(adapter.post_rdma_write(nullptr, sges, rb, {}, 0) == 0);
    REQUIRE(adapter.post_rdma_read(nullptr, sges, rb, 0) == 0);

    REQUIRE(impl.counts.sends.load() == 1);
    REQUIRE(impl.counts.recvs.load() == 1);
    REQUIRE(impl.counts.writes.load() == 1);
    REQUIRE(impl.counts.reads.load() == 1);
}

TEST_CASE("connection<Backend> stores qp, dispatcher, and config",
          "[rdma][connection]") {
    dispatcher disp;
    int qp_value = 42;
    connection_config cfg{.max_inline_data = 256};

    connection<static_test_backend> c{&qp_value, disp, cfg};

    REQUIRE(c.qp() == &qp_value);
    REQUIRE(&c.dispatcher_ref() == &disp);
    REQUIRE(c.config().max_inline_data == 256);
}

TEST_CASE("connection<polymorphic_backend> stores qp, backend, dispatcher",
          "[rdma][connection][polymorphic]") {
    dispatcher disp;
    mock_polymorphic impl{};
    int qp_value = 7;

    connection<polymorphic_backend> c{&qp_value, impl, disp,
                                       connection_config{.max_inline_data = 64}};

    REQUIRE(c.qp() == &qp_value);
    REQUIRE(&c.backend() == &impl);
    REQUIRE(&c.dispatcher_ref() == &disp);
    REQUIRE(c.config().max_inline_data == 64);
}
