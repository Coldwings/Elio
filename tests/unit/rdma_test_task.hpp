#pragma once

#include <elio/coro/task.hpp>

namespace elio::test {

// Tests own the frame until their chosen scope ends, including while the operation is pending.
inline coro::task<void> start_rdma_task(coro::task<void> operation) {
    auto handle = coro::detail::task_access::handle(operation);
    coro::detail::frame_context_scope frame(&handle.promise());
    handle.resume();
    return operation;
}

} // namespace elio::test
