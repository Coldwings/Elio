#include <elio/io/file_transfer.hpp>

// Link a direct-header consumer without an umbrella or extra runtime include.
int main() {
    auto operation = elio::io::pread_exactly(-1, {}, 0);
    auto handle = elio::coro::detail::task_access::handle(operation);
    handle.resume();
    const auto result = operation.await_resume();
    return result.end == elio::io::transfer_end::complete &&
           result.transferred == 0 && !result.error ? 0 : 1;
}
