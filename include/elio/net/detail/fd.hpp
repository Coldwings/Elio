#pragma once

#include <fcntl.h>

namespace elio::net::detail {

inline bool set_nonblocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    if ((flags & O_NONBLOCK) != 0) {
        return true;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

} // namespace elio::net::detail
