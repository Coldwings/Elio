/// @file async_file_copy.cpp
/// @brief Async File I/O Example
/// 
/// This example demonstrates asynchronous file operations using Elio's
/// io_uring/epoll backend. It performs concurrent file copying and
/// shows how to use async read/write operations.
///
/// Usage: ./async_file_copy <source> <dest>
///        ./async_file_copy --benchmark <file_size_mb>

#include <elio/elio.hpp>
#include <iostream>
#include <fstream>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

using namespace elio;
using namespace elio::coro;
using namespace elio::runtime;

/// Async file copy using io_uring/epoll
task<bool> async_copy_file(const std::string& src_path, const std::string& dst_path) {
    auto& ctx = io::default_io_context();
    
    // Open source file
    int src_fd = open(src_path.c_str(), O_RDONLY);
    if (src_fd < 0) {
        std::cerr << "Failed to open source file: " << strerror(errno) << std::endl;
        co_return false;
    }
    
    // Get file size
    struct stat st;
    if (fstat(src_fd, &st) < 0) {
        std::cerr << "Failed to stat source file: " << strerror(errno) << std::endl;
        close(src_fd);
        co_return false;
    }
    size_t file_size = st.st_size;
    
    // Open destination file
    int dst_fd = open(dst_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        std::cerr << "Failed to open destination file: " << strerror(errno) << std::endl;
        close(src_fd);
        co_return false;
    }
    
    std::cout << "Copying " << file_size << " bytes..." << std::endl;
    
    constexpr size_t BUFFER_SIZE = 64 * 1024;  // 64KB buffer
    std::vector<char> buffer(BUFFER_SIZE);
    
    size_t total_copied = 0;
    int64_t offset = 0;
    
    auto start = std::chrono::steady_clock::now();
    
    while (total_copied < file_size) {
        size_t to_read = std::min(BUFFER_SIZE, file_size - total_copied);
        
        // Async read from source
        auto read_result = co_await io::async_read(ctx, src_fd, buffer.data(), to_read, offset);
        
        if (read_result.result <= 0) {
            if (read_result.result == 0) {
                break;  // EOF
            }
            std::cerr << "Read error: " << strerror(-read_result.result) << std::endl;
            close(src_fd);
            close(dst_fd);
            co_return false;
        }
        
        size_t bytes_read = read_result.result;
        
        // Async write to destination
        auto write_result = co_await io::async_write(ctx, dst_fd, buffer.data(), bytes_read, offset);
        
        if (write_result.result <= 0) {
            std::cerr << "Write error: " << strerror(-write_result.result) << std::endl;
            close(src_fd);
            close(dst_fd);
            co_return false;
        }
        
        total_copied += bytes_read;
        offset += bytes_read;
        
        // Progress indicator
        int percent = static_cast<int>((total_copied * 100) / file_size);
        std::cout << "\rProgress: " << percent << "% (" << total_copied << "/" << file_size << " bytes)" << std::flush;
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    // Close files
    co_await io::async_close(ctx, src_fd);
    co_await io::async_close(ctx, dst_fd);
    
    std::cout << std::endl;
    std::cout << "Copy completed in " << duration_ms << " ms" << std::endl;
    
    if (duration_ms > 0) {
        double throughput_mbs = (total_copied / (1024.0 * 1024.0)) / (duration_ms / 1000.0);
        std::cout << "Throughput: " << throughput_mbs << " MB/s" << std::endl;
    }
    
    co_return true;
}

/// Concurrent file read demonstration
task<void> concurrent_read_demo(const std::vector<std::string>& files) {
    auto& ctx = io::default_io_context();
    
    std::cout << "Reading " << files.size() << " files concurrently..." << std::endl;
    
    struct FileInfo {
        int fd;
        std::string path;
        std::vector<char> buffer;
        size_t size;
    };
    
    std::vector<FileInfo> file_infos;
    
    // Open all files
    for (const auto& path : files) {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            std::cerr << "Failed to open: " << path << std::endl;
            continue;
        }
        
        struct stat st;
        if (fstat(fd, &st) == 0) {
            file_infos.push_back({fd, path, std::vector<char>(st.st_size), static_cast<size_t>(st.st_size)});
        } else {
            close(fd);
        }
    }
    
    auto start = std::chrono::steady_clock::now();
    
    // Read all files (would be more concurrent with multiple coroutines)
    size_t total_bytes = 0;
    for (auto& info : file_infos) {
        auto result = co_await io::async_read(ctx, info.fd, info.buffer.data(), info.size, 0);
        if (result.result > 0) {
            total_bytes += result.result;
            std::cout << "  Read " << result.result << " bytes from " << info.path << std::endl;
        }
        close(info.fd);
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    std::cout << "Total: " << total_bytes << " bytes in " << duration_ms << " ms" << std::endl;
    
    co_return;
}

/// Benchmark async vs sync file I/O
task<void> benchmark_io(size_t file_size_mb) {
    auto& ctx = io::default_io_context();
    
    const std::string test_file = "/tmp/elio_benchmark_test.dat";
    size_t file_size = file_size_mb * 1024 * 1024;
    
    std::cout << "Benchmark: " << file_size_mb << " MB file" << std::endl;
    std::cout << "I/O Backend: " << ctx.get_backend_name() << std::endl;
    std::cout << std::endl;
    
    // Create test file
    std::cout << "Creating test file..." << std::endl;
    {
        std::ofstream ofs(test_file, std::ios::binary);
        std::vector<char> data(1024 * 1024, 'X');  // 1MB of data
        for (size_t i = 0; i < file_size_mb; ++i) {
            ofs.write(data.data(), data.size());
        }
    }
    
    constexpr size_t BUFFER_SIZE = 64 * 1024;
    std::vector<char> buffer(BUFFER_SIZE);
    
    // Async read benchmark
    {
        int fd = open(test_file.c_str(), O_RDONLY);
        if (fd < 0) {
            std::cerr << "Failed to open test file" << std::endl;
            co_return;
        }
        
        auto start = std::chrono::steady_clock::now();
        
        size_t total_read = 0;
        int64_t offset = 0;
        while (total_read < file_size) {
            auto result = co_await io::async_read(ctx, fd, buffer.data(), BUFFER_SIZE, offset);
            if (result.result <= 0) break;
            total_read += result.result;
            offset += result.result;
        }
        
        auto end = std::chrono::steady_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        close(fd);
        
        double throughput = (total_read / (1024.0 * 1024.0)) / (duration_ms / 1000.0);
        std::cout << "Async read:  " << duration_ms << " ms (" << throughput << " MB/s)" << std::endl;
    }
    
    // Sync read benchmark for comparison
    {
        int fd = open(test_file.c_str(), O_RDONLY);
        if (fd < 0) {
            std::cerr << "Failed to open test file" << std::endl;
            co_return;
        }
        
        auto start = std::chrono::steady_clock::now();
        
        size_t total_read = 0;
        while (total_read < file_size) {
            ssize_t result = read(fd, buffer.data(), BUFFER_SIZE);
            if (result <= 0) break;
            total_read += result;
        }
        
        auto end = std::chrono::steady_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        close(fd);
        
        double throughput = (total_read / (1024.0 * 1024.0)) / (duration_ms / 1000.0);
        std::cout << "Sync read:   " << duration_ms << " ms (" << throughput << " MB/s)" << std::endl;
    }
    
    // Clean up
    unlink(test_file.c_str());
    
    co_return;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Elio Async File I/O Example" << std::endl;
        std::cout << std::endl;
        std::cout << "Usage:" << std::endl;
        std::cout << "  " << argv[0] << " <source> <dest>      - Copy file" << std::endl;
        std::cout << "  " << argv[0] << " --benchmark <MB>     - Run I/O benchmark" << std::endl;
        std::cout << "  " << argv[0] << " --read <file1> ...   - Read multiple files" << std::endl;
        return 1;
    }
    
    scheduler sched(2);
    
    // Set the I/O context so workers can poll for I/O completions
    sched.set_io_context(&io::default_io_context());
    
    sched.start();
    
    std::atomic<bool> done{false};
    int result = 0;
    
    std::string mode = argv[1];
    
    if (mode == "--benchmark") {
        size_t size_mb = (argc > 2) ? std::stoul(argv[2]) : 100;
        
        auto run = [&]() -> task<void> {
            co_await benchmark_io(size_mb);
            done = true;
        };
        
        auto t = run();
        sched.spawn(t.release());
    } else if (mode == "--read") {
        std::vector<std::string> files;
        for (int i = 2; i < argc; ++i) {
            files.push_back(argv[i]);
        }
        
        auto run = [&]() -> task<void> {
            co_await concurrent_read_demo(files);
            done = true;
        };
        
        auto t = run();
        sched.spawn(t.release());
    } else if (argc >= 3) {
        // File copy mode
        std::string src = argv[1];
        std::string dst = argv[2];
        
        auto run = [&]() -> task<void> {
            bool success = co_await async_copy_file(src, dst);
            result = success ? 0 : 1;
            done = true;
        };
        
        auto t = run();
        sched.spawn(t.release());
    } else {
        std::cerr << "Invalid arguments" << std::endl;
        return 1;
    }
    
    // Wait for completion
    while (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    
    return result;
}
