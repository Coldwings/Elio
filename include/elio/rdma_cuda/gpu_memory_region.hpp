#pragma once

#include <cstddef>
#include <utility>

#include <elio/rdma_cuda/gpu_buffer.hpp>
#include <elio/rdma/memory_region.hpp>
#include <elio/rdma_ibverbs/ibverbs_backend.hpp>

namespace elio::rdma_cuda {

/// Combines GPU device memory allocation with RDMA memory registration.
/// Destruction order: MR deregistered first, then cudaFree (C++ member
/// destruction is reverse-declaration-order).
class gpu_memory_region {
public:
    gpu_memory_region() = default;

    gpu_memory_region(void* pd, std::size_t size, int access)
        : buf_(size),
          mr_(pd, buf_.data(), buf_.size(), access) {}

    gpu_memory_region(gpu_memory_region&&) = default;
    gpu_memory_region& operator=(gpu_memory_region&& other) noexcept {
        if (this != &other) {
            // Deregister the old MR before gpu_buffer frees the old CUDA allocation.
            mr_ = std::move(other.mr_);
            buf_ = std::move(other.buf_);
        }
        return *this;
    }
    gpu_memory_region(const gpu_memory_region&) = delete;
    gpu_memory_region& operator=(const gpu_memory_region&) = delete;

    void* gpu_data() const noexcept { return buf_.data(); }
    std::size_t size() const noexcept { return buf_.size(); }
    bool ok() const noexcept { return static_cast<bool>(buf_) && mr_.ok(); }

    elio::rdma::buffer_view view() const noexcept {
        return mr_.view();
    }

    elio::rdma::buffer_view view(std::size_t offset,
                                  std::size_t len) const noexcept {
        return mr_.view(offset, len);
    }

    elio::rdma::remote_buffer remote() const noexcept {
        return mr_.remote();
    }

    elio::rdma::remote_buffer remote(std::size_t offset,
                                      std::size_t len) const noexcept {
        return mr_.remote(offset, len);
    }

    uint32_t lkey() const noexcept { return mr_.lkey(); }
    uint32_t rkey() const noexcept { return mr_.rkey(); }

private:
    gpu_buffer buf_;
    elio::rdma::memory_region<elio::rdma_ibverbs::ibverbs_backend> mr_;
};

} // namespace elio::rdma_cuda
