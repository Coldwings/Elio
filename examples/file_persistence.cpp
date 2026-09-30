#include <elio/elio.hpp>

#include <cstdlib>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

elio::coro::task<int> main_task() {
    char path[] = "/tmp/elio_persistence_example_XXXXXX";
    elio::io::fd_guard fd(::mkstemp(path));
    if (fd.get() < 0) co_return 1;
    ::unlink(path);

    auto allocated = co_await elio::io::allocate_file_range(fd.get(), 0, 0, 4096);
    if (!allocated && allocated.error_value() != EOPNOTSUPP &&
            allocated.error_value() != ENOSYS) {
        std::cerr << "Allocation failed: " << allocated.error.message() << '\n';
        co_return 1;
    }
    // Application ordering is explicit; none of these calls is a transaction.
    auto resized = co_await elio::io::truncate_file(fd.get(), 1024);
    if (!resized) co_return 1;
    auto synced = co_await elio::io::sync_file(fd.get());
    if (!synced) co_return 1;
    struct stat metadata{};
    if (::fstat(fd.get(), &metadata) != 0 || metadata.st_size != 1024) co_return 1;
    std::cout << "Size: " << metadata.st_size << ", synced: yes\n";
    co_return 0;
}

int main() {
    return elio::run(main_task);
}
