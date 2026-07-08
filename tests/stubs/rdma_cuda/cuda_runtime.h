#pragma once

#include <cstddef>

using cudaError_t = int;

inline constexpr cudaError_t cudaSuccess = 0;
inline constexpr cudaError_t cudaErrorMemoryAllocation = 2;

extern "C" void* elio_rdma_cuda_test_cuda_malloc(std::size_t size);
extern "C" void elio_rdma_cuda_test_cuda_free(void* ptr);
extern "C" const char* elio_rdma_cuda_test_cuda_error_string(cudaError_t error);

inline cudaError_t cudaMalloc(void** ptr, std::size_t size) {
    *ptr = elio_rdma_cuda_test_cuda_malloc(size);
    return *ptr ? cudaSuccess : cudaErrorMemoryAllocation;
}

inline cudaError_t cudaFree(void* ptr) {
    elio_rdma_cuda_test_cuda_free(ptr);
    return cudaSuccess;
}

inline const char* cudaGetErrorString(cudaError_t error) {
    return elio_rdma_cuda_test_cuda_error_string(error);
}
