#include <catch2/catch_test_macros.hpp>

#include <elio/rdma_cuda/gpu_memory_region.hpp>

#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {

enum class event_kind {
    dereg_mr,
    cuda_free,
};

struct event {
    event_kind kind;
    void*      addr;
};

struct test_state {
    std::vector<event> events;
    std::uint32_t      next_key = 1;
};

test_state* active_state = nullptr;

}  // namespace

extern "C" void* elio_rdma_cuda_test_cuda_malloc(std::size_t size) {
    return ::operator new(size == 0 ? 1 : size, std::nothrow);
}

extern "C" void elio_rdma_cuda_test_cuda_free(void* ptr) {
    if (active_state) {
        active_state->events.push_back({event_kind::cuda_free, ptr});
    }
    ::operator delete(ptr);
}

extern "C" const char* elio_rdma_cuda_test_cuda_error_string(cudaError_t) {
    return "stub cuda error";
}

extern "C" ibv_mr* elio_rdma_cuda_test_ibv_reg_mr(
    ibv_pd*, void* addr, std::size_t length, int) {
    auto* mr = new (std::nothrow) ibv_mr{};
    if (!mr) return nullptr;
    mr->addr = addr;
    mr->length = length;
    if (active_state) {
        mr->lkey = active_state->next_key++;
        mr->rkey = active_state->next_key++;
    }
    return mr;
}

extern "C" int elio_rdma_cuda_test_ibv_dereg_mr(ibv_mr* mr) {
    if (active_state) {
        active_state->events.push_back({event_kind::dereg_mr, mr->addr});
    }
    delete mr;
    return 0;
}

TEST_CASE("gpu_memory_region move assignment deregisters old MR before cudaFree",
          "[rdma][cuda][move]") {
    test_state state;
    active_state = &state;

    {
        elio::rdma_cuda::gpu_memory_region a{
            nullptr, 64, /*access=*/0x7};
        void* const old_addr = a.gpu_data();

        elio::rdma_cuda::gpu_memory_region b{
            nullptr, 128, /*access=*/0x7};
        void* const new_addr = b.gpu_data();

        REQUIRE(old_addr != nullptr);
        REQUIRE(new_addr != nullptr);
        REQUIRE(old_addr != new_addr);

        state.events.clear();

        a = std::move(b);

        REQUIRE(a.gpu_data() == new_addr);
        REQUIRE(state.events.size() == 2);
        REQUIRE(state.events[0].kind == event_kind::dereg_mr);
        REQUIRE(state.events[0].addr == old_addr);
        REQUIRE(state.events[1].kind == event_kind::cuda_free);
        REQUIRE(state.events[1].addr == old_addr);
    }

    active_state = nullptr;
}
