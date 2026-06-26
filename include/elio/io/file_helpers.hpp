#pragma once

#include "io_awaitables.hpp"
#include <elio/coro/task.hpp>
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <system_error>

namespace elio::io {

/// RAII wrapper for file descriptors to ensure cleanup
class fd_guard {
public:
    explicit fd_guard(int fd) noexcept : fd_(fd) {}

    ~fd_guard() {
        if (fd_ >= 0) {
            close_fd_for_destructor(fd_);
        }
    }

    // Non-copyable
    fd_guard(const fd_guard&) = delete;
    fd_guard& operator=(const fd_guard&) = delete;

    // Movable
    fd_guard(fd_guard&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    fd_guard& operator=(fd_guard&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                close_fd_for_destructor(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const noexcept { return fd_; }

    int release() noexcept {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_;
};

/// Read entire file content into a string
/// Returns nullopt if file cannot be opened or read
inline coro::task<std::optional<std::string>> read_file(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        co_return std::nullopt;
    }

    // Use RAII to ensure fd is always closed
    fd_guard guard(fd);

    // Get initial size estimate, but don't rely on it for correctness
    struct stat st;
    size_t initial_size = 0;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        initial_size = static_cast<size_t>(st.st_size);
    }

    // Read the file in chunks, handling TOCTOU where file size may change
    std::string buffer;
    buffer.reserve(initial_size > 0 ? initial_size : 4096);

    size_t total_read = 0;
    while (true) {
        // Ensure we have space to read
        size_t capacity = buffer.capacity();
        if (total_read >= capacity) {
            // Double the buffer size
            buffer.resize(capacity * 2);
        } else {
            buffer.resize(capacity);
        }

        // Read a chunk
        size_t chunk = std::min(buffer.size() - total_read, size_t(1024 * 1024)); // 1MB chunks
        auto result = co_await async_read(fd,
            buffer.data() + total_read, chunk,
            static_cast<int64_t>(total_read));

        if (!result.success() || result.result == 0) {
            // EOF or error - truncate to actual read size
            buffer.resize(total_read);
            buffer.shrink_to_fit();
            if (total_read == 0 && !result.success()) {
                co_return std::nullopt;
            }
            co_return buffer;
        }

        total_read += static_cast<size_t>(result.result);
    }

    co_return buffer;
}

/// Write entire content to file (creates/truncates)
/// Returns true on success
inline coro::task<bool> write_file(const std::string& path, const std::string& data) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        co_return false;
    }

    // Use RAII to ensure fd is always closed
    fd_guard guard(fd);

    size_t total_written = 0;
    while (total_written < data.size()) {
        size_t remaining = data.size() - total_written;
        size_t chunk = std::min(remaining, size_t(1024 * 1024)); // 1MB chunks

        auto result = co_await async_write(fd,
            data.data() + total_written, chunk,
            static_cast<int64_t>(total_written));

        if (!result.success() || result.result == 0) {
            co_return false;
        }

        total_written += static_cast<size_t>(result.result);
    }

    co_return true;
}

/// Append content to file (creates if not exists)
/// Returns true on success
inline coro::task<bool> append_file(const std::string& path, const std::string& data) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        co_return false;
    }

    // Use RAII to ensure fd is always closed
    fd_guard guard(fd);

    size_t total_written = 0;
    while (total_written < data.size()) {
        size_t remaining = data.size() - total_written;
        size_t chunk = std::min(remaining, size_t(1024 * 1024)); // 1MB chunks

        // For append, use offset=-1 which io_uring translates to current position
        // The O_APPEND flag ensures writes go to end regardless of offset
        auto result = co_await async_write(fd,
            data.data() + total_written, chunk, -1);

        if (!result.success() || result.result == 0) {
            co_return false;
        }

        total_written += static_cast<size_t>(result.result);
    }

    co_return true;
}

/// Check if a file exists
inline bool file_exists(const std::string& path) noexcept {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

/// Get file size in bytes, or nullopt if file doesn't exist or error
inline std::optional<int64_t> file_size(const std::string& path) noexcept {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return std::nullopt;
    }
    return static_cast<int64_t>(st.st_size);
}

/// Directory entry returned by read_dir
struct dir_entry {
    std::string name;       ///< Entry name (filename)
    bool is_dir;            ///< True if directory
    bool is_file;           ///< True if regular file
    bool is_symlink;        ///< True if symbolic link
};

/// Read directory contents
/// Returns list of entries, or nullopt on error
inline std::optional<std::vector<dir_entry>> read_dir(const std::string& path) {
    std::vector<dir_entry> entries;
    
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        return std::nullopt;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name(entry->d_name);
        
        // Skip . and ..
        if (name == "." || name == "..") {
            continue;
        }
        
        dir_entry e;
        e.name = std::move(name);
        e.is_dir = false;
        e.is_file = false;
        e.is_symlink = false;
        
#ifdef _DIRENT_HAVE_D_TYPE
        switch (entry->d_type) {
            case DT_DIR:  e.is_dir = true; break;
            case DT_REG:  e.is_file = true; break;
            case DT_LNK:  e.is_symlink = true; break;
            default: break;
        }
#else
        // Fallback: stat each entry
        std::string full_path = path;
        if (!full_path.empty() && full_path.back() != '/') {
            full_path += '/';
        }
        full_path += e.name;
        struct stat st;
        if (stat(full_path.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) e.is_dir = true;
            else if (S_ISREG(st.st_mode)) e.is_file = true;
            else if (S_ISLNK(st.st_mode)) e.is_symlink = true;
        }
#endif
        
        entries.push_back(std::move(e));
    }
    
    closedir(dir);
    return entries;
}

} // namespace elio::io
