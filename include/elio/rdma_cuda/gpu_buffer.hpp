#pragma once

#include <cstddef>
#include <stdexcept>
#include <utility>

#include <cuda_runtime.h>

namespace elio::rdma_cuda {

class gpu_buffer {
public:
    gpu_buffer() = default;

    explicit gpu_buffer(std::size_t size) : size_(size) {
        auto rc = ::cudaMalloc(&ptr_, size_);
        if (rc != cudaSuccess)
            throw std::runtime_error(
                std::string("cudaMalloc failed: ") +
                ::cudaGetErrorString(rc));
    }

    ~gpu_buffer() noexcept { reset(); }

    gpu_buffer(gpu_buffer&& o) noexcept
        : ptr_(std::exchange(o.ptr_, nullptr)),
          size_(std::exchange(o.size_, 0)) {}

    gpu_buffer& operator=(gpu_buffer&& o) noexcept {
        if (this != &o) {
            reset();
            ptr_  = std::exchange(o.ptr_, nullptr);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }

    gpu_buffer(const gpu_buffer&) = delete;
    gpu_buffer& operator=(const gpu_buffer&) = delete;

    void* data() const noexcept { return ptr_; }
    std::size_t size() const noexcept { return size_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    void reset() noexcept {
        if (ptr_) {
            ::cudaFree(ptr_);
            ptr_ = nullptr;
            size_ = 0;
        }
    }

    void* ptr_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace elio::rdma_cuda
