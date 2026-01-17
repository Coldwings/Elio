#include <catch2/catch_test_macros.hpp>
#include <elio/coro/awaitable_base.hpp>
#include <elio/coro/task.hpp>
#include <coroutine>

using namespace elio::coro;

// Test awaitable that returns an int
class test_awaitable : public awaitable_base<test_awaitable> {
public:
    explicit test_awaitable(int value) : value_(value) {}
    
    bool await_ready_impl() const noexcept {
        return ready_;
    }
    
    void await_suspend_impl(std::coroutine_handle<> awaiter) noexcept {
        awaiter_ = awaiter;
        // In a real scenario, we'd schedule the awaiter to resume later
        // For testing, we'll resume immediately
        awaiter_.resume();
    }
    
    int await_resume_impl() {
        return value_;
    }
    
    void set_ready(bool ready) { ready_ = ready; }

private:
    int value_;
    bool ready_ = false;
    std::coroutine_handle<> awaiter_;
};

// Test awaitable that returns void
class void_awaitable : public awaitable_base<void_awaitable> {
public:
    bool await_ready_impl() const noexcept {
        return false;
    }
    
    void await_suspend_impl(std::coroutine_handle<> awaiter) noexcept {
        awaiter.resume();
    }
    
    void await_resume_impl() {}
};

TEST_CASE("awaitable_base forwards await_ready", "[awaitable_base]") {
    test_awaitable awaitable(42);
    
    REQUIRE(awaitable.await_ready() == false);
    
    awaitable.set_ready(true);
    REQUIRE(awaitable.await_ready() == true);
}

TEST_CASE("awaitable_base forwards await_suspend", "[awaitable_base]") {
    test_awaitable awaitable(42);
    
    // Create a dummy coroutine handle (we won't actually use it)
    bool suspended = false;
    
    auto coro = [&]() -> task<void> {
        suspended = true;
        co_await awaitable;
    };
    
    auto t = coro();
    t.handle().resume();
    
    REQUIRE(suspended == true);
}

TEST_CASE("awaitable_base forwards await_resume with return value", "[awaitable_base]") {
    test_awaitable awaitable(123);
    
    auto coro = [&]() -> task<int> {
        int result = co_await awaitable;
        co_return result;
    };
    
    auto t = coro();
    t.handle().resume();
    
    REQUIRE(t.handle().promise().value_.value() == 123);
}

TEST_CASE("awaitable_base works with void return", "[awaitable_base]") {
    void_awaitable awaitable;
    
    auto coro = [&]() -> task<void> {
        co_await awaitable;
        co_return;
    };
    
    auto t = coro();
    t.handle().resume();
    
    REQUIRE(t.handle().done());
}

TEST_CASE("awaitable_base in nested coroutines", "[awaitable_base]") {
    auto inner = []() -> task<int> {
        test_awaitable awaitable(50);
        int value = co_await awaitable;
        co_return value * 2;
    };
    
    auto outer = [&]() -> task<int> {
        int value = co_await inner();
        co_return value + 10;
    };
    
    auto t = outer();
    t.handle().resume();
    
    // Should be (50 * 2) + 10 = 110
    REQUIRE(t.handle().promise().value_.value() == 110);
}

// Test awaitable with symmetric transfer
class symmetric_awaitable : public awaitable_base<symmetric_awaitable> {
public:
    explicit symmetric_awaitable(int value) : value_(value) {}
    
    bool await_ready_impl() const noexcept {
        return false;
    }
    
    std::coroutine_handle<> await_suspend_impl(std::coroutine_handle<> awaiter) noexcept {
        // Return the awaiter for symmetric transfer
        return awaiter;
    }
    
    int await_resume_impl() {
        return value_;
    }

private:
    int value_;
};

TEST_CASE("awaitable_base supports symmetric transfer", "[awaitable_base]") {
    auto coro = []() -> task<int> {
        symmetric_awaitable awaitable(999);
        int result = co_await awaitable;
        co_return result;
    };
    
    auto t = coro();
    t.handle().resume();
    
    REQUIRE(t.handle().promise().value_.value() == 999);
}
