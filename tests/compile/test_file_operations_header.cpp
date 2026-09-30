#include <elio/io/file_operations.hpp>

int main() {
    auto operation = elio::io::sync_file(-1);
    auto handle = elio::coro::detail::task_access::handle(operation);
    handle.resume();
    const auto result = operation.await_resume();
    return result.end == elio::io::file_operation_end::error &&
           result.error_value() == ENOTSUP ? 0 : 1;
}
