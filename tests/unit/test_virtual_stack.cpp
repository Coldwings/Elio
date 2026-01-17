#include <catch2/catch_test_macros.hpp>
#include <elio/coro/promise_base.hpp>
#include <elio/coro/frame.hpp>
#include <thread>
#include <vector>
#include <memory>

using namespace elio::coro;

TEST_CASE("promise_base constructor links to parent", "[virtual_stack]") {
    REQUIRE(promise_base::current_frame() == nullptr);
    
    promise_base frame1;
    REQUIRE(promise_base::current_frame() == &frame1);
    REQUIRE(frame1.parent() == nullptr);
    
    promise_base frame2;
    REQUIRE(promise_base::current_frame() == &frame2);
    REQUIRE(frame2.parent() == &frame1);
    
    promise_base frame3;
    REQUIRE(promise_base::current_frame() == &frame3);
    REQUIRE(frame3.parent() == &frame2);
}

TEST_CASE("promise_base destructor restores parent", "[virtual_stack]") {
    REQUIRE(promise_base::current_frame() == nullptr);
    
    promise_base frame1;
    REQUIRE(promise_base::current_frame() == &frame1);
    
    {
        promise_base frame2;
        REQUIRE(promise_base::current_frame() == &frame2);
        
        {
            promise_base frame3;
            REQUIRE(promise_base::current_frame() == &frame3);
        }
        
        // After frame3 destroyed, frame2 should be current
        REQUIRE(promise_base::current_frame() == &frame2);
    }
    
    // After frame2 destroyed, frame1 should be current
    REQUIRE(promise_base::current_frame() == &frame1);
}

TEST_CASE("promise_base stores exceptions", "[virtual_stack]") {
    promise_base frame;
    
    REQUIRE(frame.exception() == nullptr);
    
    try {
        throw std::runtime_error("test error");
    } catch (...) {
        frame.unhandled_exception();
    }
    
    REQUIRE(frame.exception() != nullptr);
    
    // Verify we can rethrow
    try {
        std::rethrow_exception(frame.exception());
        FAIL("Should have thrown");
    } catch (const std::runtime_error& e) {
        REQUIRE(std::string(e.what()) == "test error");
    }
}

TEST_CASE("Virtual stack depth calculation", "[virtual_stack]") {
    REQUIRE(get_stack_depth() == 0);
    
    promise_base frame1;
    REQUIRE(get_stack_depth() == 1);
    
    {
        promise_base frame2;
        REQUIRE(get_stack_depth() == 2);
        
        {
            promise_base frame3;
            REQUIRE(get_stack_depth() == 3);
        }
        
        REQUIRE(get_stack_depth() == 2);
    }
    
    REQUIRE(get_stack_depth() == 1);
}

TEST_CASE("Virtual stack dump", "[virtual_stack]") {
    auto frames = dump_virtual_stack();
    REQUIRE(frames.empty());
    
    promise_base frame1;
    frames = dump_virtual_stack();
    REQUIRE(frames.size() == 1);
    
    {
        promise_base frame2;
        frames = dump_virtual_stack();
        REQUIRE(frames.size() == 2);
        
        {
            promise_base frame3;
            frames = dump_virtual_stack();
            REQUIRE(frames.size() == 3);
        }
    }
}

TEST_CASE("Thread-local virtual stacks are independent", "[virtual_stack]") {
    promise_base main_frame;
    REQUIRE(get_stack_depth() == 1);
    
    std::thread worker([&]() {
        // Worker thread should have empty stack
        REQUIRE(promise_base::current_frame() == nullptr);
        REQUIRE(get_stack_depth() == 0);
        
        promise_base worker_frame1;
        REQUIRE(get_stack_depth() == 1);
        
        {
            promise_base worker_frame2;
            REQUIRE(get_stack_depth() == 2);
        }
        
        REQUIRE(get_stack_depth() == 1);
    });
    
    worker.join();
    
    // Main thread stack should be unchanged
    REQUIRE(get_stack_depth() == 1);
    REQUIRE(promise_base::current_frame() == &main_frame);
}

TEST_CASE("Multiple concurrent stacks in different threads", "[virtual_stack]") {
    std::vector<std::thread> threads;
    const int num_threads = 5;
    const int stack_depth = 10;
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([stack_depth]() {
            std::vector<std::unique_ptr<promise_base>> frames;
            frames.reserve(stack_depth);
            
            for (int j = 0; j < stack_depth; ++j) {
                frames.push_back(std::make_unique<promise_base>());
                REQUIRE(get_stack_depth() == static_cast<size_t>(j + 1));
            }
            
            REQUIRE(get_stack_depth() == static_cast<size_t>(stack_depth));
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
}
