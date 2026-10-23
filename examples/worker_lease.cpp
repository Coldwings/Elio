#include <elio/runtime/worker_lease.hpp>
#include <elio/sync/event.hpp>
#include <future>
#include <iostream>
#include <stdexcept>

int main() {
    elio::runtime::scheduler sched(2);
    elio::sync::event retire_owner;
    std::promise<void> ready;
    auto ready_result = ready.get_future();
    std::exception_ptr failure;
    sched.start();
    sched.go_to(1, [&]() -> elio::coro::task<void> {
        bool published = false;
        try {
            auto owner = co_await elio::runtime::lease_current_worker();
            const auto original_thread = std::this_thread::get_id();
            ready.set_value();
            published = true;
            // Represents an external owner whose lifetime spans a non-I/O wait.
            co_await retire_owner.wait();
            if (std::this_thread::get_id() != original_thread) {
                throw std::runtime_error("owner migrated during retirement");
            }
            std::cout << "Owner cleanup on worker " << owner.worker_id() << '\n';
            // Destroy external thread-affine resources before releasing.
            owner.release();
        } catch (...) {
            if (!published) ready.set_exception(std::current_exception());
            else failure = std::current_exception();
        }
    });
    // Blocking waits are on the controller thread, never a scheduler worker.
    ready_result.get();
    sched.set_thread_count(1);
    retire_owner.set();
    const bool drained = sched.shutdown();
    if (failure) std::rethrow_exception(failure);
    return drained ? 0 : 1;
}
