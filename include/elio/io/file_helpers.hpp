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

/// Read entire file content into a string
/// Returns nullopt if file cannot be opened or read
inline coro::task<std::optional<std::string>> read_file(const std::string& path) {
    // Get file size first
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        co_return std::nullopt;
    }
    
    if (st.st_size == 0) {
        co_return std::string{};
    }
    
    auto file_size = static_cast<size_t>(st.st_size);
    std::string buffer(file_size, '\0');
    
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        co_return std::nullopt;
    }
    
    // Read in chunks to handle large files
    size_t total_read = 0;
    while (total_read < file_size) {
        size_t remaining = file_size - total_read;
        size_t chunk = std::min(remaining, size_t(1024 * 1024)); // 1MB chunks
        
        auto result = co_await async_read(fd,
            buffer.data() + total_read, chunk,
            static_cast<int64_t>(total_read));
        
        if (!result.success() || result.result == 0) {
            // EOF or error - truncate to actual read size
            buffer.resize(total_read);
            close(fd);
            if (total_read == 0 && !result.success()) {
                co_return std::nullopt;
            }
            co_return buffer;
        }
        
        total_read += static_cast<size_t>(result.result);
    }
    
    close(fd);
    co_return buffer;
}

/// Write entire content to file (creates/truncates)
/// Returns true on success
inline coro::task<bool> write_file(const std::string& path, const std::string& data) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        co_return false;
    }
    
    size_t total_written = 0;
    while (total_written < data.size()) {
        size_t remaining = data.size() - total_written;
        size_t chunk = std::min(remaining, size_t(1024 * 1024)); // 1MB chunks
        
        auto result = co_await async_write(fd,
            data.data() + total_written, chunk,
            static_cast<int64_t>(total_written));
        
        if (!result.success() || result.result == 0) {
            close(fd);
            co_return false;
        }
        
        total_written += static_cast<size_t>(result.result);
    }
    
    close(fd);
    co_return true;
}

/// Append content to file (creates if not exists)
/// Returns true on success
inline coro::task<bool> append_file(const std::string& path, const std::string& data) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        co_return false;
    }
    
    // For append, use current position (offset=-1 → offset 0 in pwrite,
    // but O_APPEND ensures writes go to end)
    auto result = co_await async_write(fd, data.data(), data.size(), -1);
    
    close(fd);
    co_return result.success() && static_cast<size_t>(result.result) == data.size();
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
