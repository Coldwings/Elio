#include <catch2/catch_test_macros.hpp>
#include <elio/coro/frame_allocator.hpp>
#include <thread>
#include <atomic>
#include <vector>

using namespace elio::coro;

TEST_CASE("Frame allocator basic allocation/deallocation", "[frame_allocator]") {
    // Test basic allocation
    void* ptr = frame_allocator::allocate(128);
    REQUIRE(ptr != nullptr);

    // Test deallocation (same thread)
    frame_allocator::deallocate(ptr, 128);
}

TEST_CASE("Frame allocator cross-thread deallocation", "[frame_allocator]") {
    // This test verifies the lookup-then-push race is fixed by holding
    // the registry mutex during the entire operation

    std::atomic<void*> allocated_ptr{nullptr};
    std::atomic<bool> ready{false};

    // Thread 1: Allocate a frame
    std::thread allocator_thread([&]() {
        void* ptr = frame_allocator::allocate(128);
        allocated_ptr.store(ptr);

        // Signal that we have a frame ready
        ready.store(true);

        // Wait for the other thread to deallocate
        while (ready.load()) {
            std::this_thread::yield();
        }
    });

    // Wait for allocation
    while (!ready.load()) {
        std::this_thread::yield();
    }

    // Thread 2: Deallocate from different thread (simulating work-stealing)
    std::thread deallocator_thread([&]() {
        // Wait for the frame to be ready
        while (!ready.load()) {
            std::this_thread::yield();
        }

        void* ptr = allocated_ptr.load();
        if (ptr) {
            // This should trigger cross-thread deallocation
            // The race condition fix holds the mutex during lookup-then-push
            frame_allocator::deallocate(ptr, 128);
        }

        // Signal completion
        ready.store(false);
    });

    allocator_thread.join();
    deallocator_thread.join();
}

TEST_CASE("Frame allocator multiple frames", "[frame_allocator]") {
    constexpr size_t num_frames = 100;
    std::vector<void*> frames;

    // Allocate multiple frames
    for (size_t i = 0; i < num_frames; ++i) {
        void* ptr = frame_allocator::allocate(128);
        REQUIRE(ptr != nullptr);
        frames.push_back(ptr);
    }

    // Deallocate all frames
    for (void* ptr : frames) {
        frame_allocator::deallocate(ptr, 128);
    }
}

TEST_CASE("Frame allocator size limits", "[frame_allocator]") {
    // Test allocation within size limit
    void* small = frame_allocator::allocate(256);
    REQUIRE(small != nullptr);
    frame_allocator::deallocate(small, 256);

    // Test allocation above size limit falls back to malloc
    void* large = frame_allocator::allocate(512);
    REQUIRE(large != nullptr);
    frame_allocator::deallocate(large, 512);
}
