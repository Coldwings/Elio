#include <catch2/catch_test_macros.hpp>

#include <elio/rdma/backend_traits.hpp>
#include <elio/rdma_ibverbs/ibverbs_backend.hpp>
#if defined(ELIO_HAS_RDMA_IBVERBS) && ELIO_HAS_RDMA_IBVERBS
#include <elio/rdma_ibverbs/cq_drain.hpp>
#endif

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>

using elio::rdma::remote_buffer;
using elio::rdma::send_flags;
using elio::rdma::sge;
using elio::rdma_ibverbs::ibverbs_backend;

static_assert(elio::rdma::backend_with_srq<ibverbs_backend>);

#if defined(ELIO_HAS_RDMA_IBVERBS) && ELIO_HAS_RDMA_IBVERBS
namespace {

struct fake_cq_verbs_api {
    static inline ibv_cq* event_cq = nullptr;
    static inline ibv_cq* acknowledged_cq = nullptr;
    static inline ibv_cq* rearmed_cq = nullptr;
    static inline ibv_cq* polled_cq = nullptr;
    static inline int poll_calls = 0;

    static int get_cq_event(ibv_comp_channel*, ibv_cq** cq,
                            void** context) noexcept {
        *cq = event_cq;
        *context = nullptr;
        return 0;
    }

    static void ack_cq_events(ibv_cq* cq, unsigned int) noexcept {
        acknowledged_cq = cq;
    }

    static int req_notify_cq(ibv_cq* cq, int) noexcept {
        rearmed_cq = cq;
        return 0;
    }

    static int poll_cq(ibv_cq* cq, int, ibv_wc*) noexcept {
        polled_cq = cq;
        ++poll_calls;
        return 0;
    }
};

} // namespace
#endif

TEST_CASE("ibverbs_backend rejects oversized SGE lists before posting",
          "[rdma][ibverbs][sge]") {
    constexpr std::size_t kOversizedCount = ibverbs_backend::kMaxSge + 1;

    std::array<char, kOversizedCount> storage{};
    std::array<sge, kOversizedCount> sges{};
    for (std::size_t i = 0; i < sges.size(); ++i) {
        sges[i] = sge{storage.data() + i, 1, 0xCAFEu};
    }

    std::span<const sge> oversized{sges};
    remote_buffer remote{0x1234u, 1, 0xBEEFu};
    send_flags flags{};

    REQUIRE(ibverbs_backend::post_send(nullptr, oversized, flags, 0, 1) ==
            -EMSGSIZE);
    REQUIRE(ibverbs_backend::post_recv(nullptr, oversized, 2) == -EMSGSIZE);
    REQUIRE(ibverbs_backend::post_srq_recv(nullptr, oversized, 7) ==
            -EMSGSIZE);
    REQUIRE(ibverbs_backend::post_rdma_write(
                nullptr, oversized, remote, flags, 0, 3) == -EMSGSIZE);
    REQUIRE(ibverbs_backend::post_rdma_read(nullptr, oversized, remote, 4) ==
            -EMSGSIZE);
    REQUIRE(ibverbs_backend::post_atomic_cas(
                nullptr, oversized, remote, 0, 1, flags, 5) == -EMSGSIZE);
    REQUIRE(ibverbs_backend::post_atomic_fetch_add(
                nullptr, oversized, remote, 1, flags, 6) == -EMSGSIZE);
}

#if defined(ELIO_HAS_RDMA_IBVERBS) && ELIO_HAS_RDMA_IBVERBS
TEST_CASE("cq drain polls the CQ returned by the channel event",
          "[rdma][ibverbs][cq][regression]") {
    auto* configured_cq = reinterpret_cast<ibv_cq*>(std::uintptr_t{0x1000});
    auto* event_cq = reinterpret_cast<ibv_cq*>(std::uintptr_t{0x2000});
    auto* channel = reinterpret_cast<ibv_comp_channel*>(std::uintptr_t{0x3000});
    fake_cq_verbs_api::event_cq = event_cq;
    fake_cq_verbs_api::acknowledged_cq = nullptr;
    fake_cq_verbs_api::rearmed_cq = nullptr;
    fake_cq_verbs_api::polled_cq = nullptr;
    fake_cq_verbs_api::poll_calls = 0;

    auto drain = elio::rdma_ibverbs::detail::make_cq_drain_impl<
        fake_cq_verbs_api>(configured_cq, channel);
    elio::rdma::dispatcher dispatcher;
    drain(dispatcher);

    REQUIRE(fake_cq_verbs_api::acknowledged_cq == event_cq);
    REQUIRE(fake_cq_verbs_api::rearmed_cq == event_cq);
    REQUIRE(fake_cq_verbs_api::polled_cq == event_cq);
    REQUIRE(fake_cq_verbs_api::poll_calls == 1);
}
#endif
