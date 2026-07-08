#include <catch2/catch_test_macros.hpp>

#include <elio/rdma_ibverbs/ibverbs_backend.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <span>

using elio::rdma::remote_buffer;
using elio::rdma::send_flags;
using elio::rdma::sge;
using elio::rdma_ibverbs::ibverbs_backend;

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
    REQUIRE(ibverbs_backend::post_rdma_write(
                nullptr, oversized, remote, flags, 0, 3) == -EMSGSIZE);
    REQUIRE(ibverbs_backend::post_rdma_read(nullptr, oversized, remote, 4) ==
            -EMSGSIZE);
    REQUIRE(ibverbs_backend::post_atomic_cas(
                nullptr, oversized, remote, 0, 1, flags, 5) == -EMSGSIZE);
    REQUIRE(ibverbs_backend::post_atomic_fetch_add(
                nullptr, oversized, remote, 1, flags, 6) == -EMSGSIZE);
}
