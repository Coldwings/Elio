#include <elio/elio.hpp>
#include <iostream>

using namespace elio;

// A simple coroutine that returns a value
coro::task<int> compute() {
    co_return 42;
}

// Main coroutine that awaits other coroutines
coro::task<int> main_task() {
    int result = co_await compute();
    std::cout << "Result: " << result << std::endl;
    co_return 0;
}

int main() {
    return elio::run(main_task);
}
