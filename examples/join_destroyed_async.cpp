#include <elio/elio.hpp>

#include <iostream>
#include <memory>

struct capture_probe {
    explicit capture_probe(bool& released) noexcept : released_(released) {}
    ~capture_probe() { released_ = true; }
    bool& released_;
};

elio::coro::task<int> main_task() {
    bool captures_released = false;
    auto handle = elio::spawn(
        [probe = std::make_unique<capture_probe>(captures_released)]()
            -> elio::coro::task<int> {
            co_return 42;
        });

    const int result = co_await handle;
    // A result can be ready before the callable wrapper releases its captures.
    co_await handle.wait_destroyed_async();
    std::cout << "Result: " << result << ", captures released: "
              << (captures_released ? "yes" : "no") << '\n';
    co_return result == 42 && captures_released ? 0 : 1;
}

int main() {
    return elio::run(main_task);
}
