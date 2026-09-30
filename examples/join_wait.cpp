#include <elio/elio.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

elio::coro::task<int> main_task() {
    auto release = std::make_shared<elio::sync::event>();
    auto handle = elio::spawn(
        [input = std::string("owned background input"), release]()
            -> elio::coro::task<size_t> {
            co_await release->wait();
            co_return input.size();
        });

    const auto outcome = co_await handle.wait_until(
        std::chrono::steady_clock::now() + std::chrono::milliseconds(20));
    // The caller keeps its background owner after observation departs. An
    // application returning here must transfer it to its own bounded owner.
    const bool still_pending = !handle.is_ready();
    release->set();
    const auto eventual = co_await handle.wait();
    const auto result = co_await handle;
    co_await handle.wait_destroyed_async();
    std::cout << "Timed out without stopping owned work: "
              << (outcome == elio::coro::join_wait_outcome::timed_out && still_pending ? "yes" : "no")
              << "; eventual result: " << result << '\n';
    co_return outcome == elio::coro::join_wait_outcome::timed_out && still_pending &&
        eventual == elio::coro::join_wait_outcome::completed && result == 22 ? 0 : 1;
}

int main() {
    return elio::run(main_task);
}
