/// @file bench_tcp_reference_client.cpp
/// @brief Dependency-neutral client for TCP benchmark server attribution.

#include "bench_tcp_common.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <netdb.h>
#include <netinet/tcp.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

int connect_peer(const bench::config& cfg) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(cfg.port);
    const int resolved = ::getaddrinfo(
        cfg.host.c_str(), service.c_str(), &hints, &addresses);
    if (resolved != 0) {
        std::fprintf(stderr, "resolve failed: %s\n", gai_strerror(resolved));
        return -1;
    }

    int socket = -1;
    for (auto* address = addresses; address != nullptr;
         address = address->ai_next) {
        socket = ::socket(address->ai_family, address->ai_socktype,
                          address->ai_protocol);
        if (socket < 0) {
            continue;
        }
        if (::connect(socket, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        ::close(socket);
        socket = -1;
    }
    ::freeaddrinfo(addresses);
    if (socket >= 0) {
        const int enabled = 1;
        (void)::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                           &enabled, sizeof(enabled));
    }
    return socket;
}

bool send_all(int socket, const uint8_t* data, std::size_t size) {
    while (size != 0) {
        const ssize_t sent = ::send(socket, data, size, MSG_NOSIGNAL);
        if (sent > 0) {
            data += sent;
            size -= static_cast<std::size_t>(sent);
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool receive_all(int socket, uint8_t* data, std::size_t size) {
    while (size != 0) {
        const ssize_t received = ::recv(socket, data, size, 0);
        if (received > 0) {
            data += received;
            size -= static_cast<std::size_t>(received);
        } else if (received < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

uint64_t elapsed_ns(clock_type::time_point begin,
                    clock_type::time_point end) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
            .count());
}

bool run_sequential_phase(int socket, bench::phase phase, uint64_t trial,
                          uint64_t count, std::size_t size,
                          bench::counters& counters,
                          std::vector<uint64_t>* latency_samples,
                          uint64_t* total_elapsed,
                          uint64_t* process_cpu_elapsed) {
    std::vector<uint8_t> outgoing(size);
    std::vector<uint8_t> incoming(size);
    if (!bench::initialize_record(outgoing)) {
        ++counters.payload_errors;
        return false;
    }
    clock_type::time_point phase_begin;
    uint64_t process_cpu_begin = 0;
    for (uint64_t sequence = 0; sequence != count; ++sequence) {
        if (!bench::stamp_record(outgoing, phase, trial, sequence)) {
            ++counters.payload_errors;
            return false;
        }
        if (sequence == 0) {
            phase_begin = clock_type::now();
            if (process_cpu_elapsed != nullptr) {
                process_cpu_begin = bench::process_cpu_time_ns();
            }
        }
        const auto sample_begin = clock_type::now();
        counters.begin_write(size, 1, size);
        if (!send_all(socket, outgoing.data(), outgoing.size())) {
            ++counters.transport_errors;
            return false;
        }
        counters.complete_write(size, 1, size);
        counters.observe_unacknowledged(1);
        if (!receive_all(socket, incoming.data(), incoming.size())) {
            ++counters.transport_errors;
            return false;
        }
        counters.receive_record(size);
        const auto validation = bench::validate_record(
            incoming, phase, trial, sequence, size);
        if (validation != bench::record_error::none) {
            ++counters.payload_errors;
            return false;
        }
        counters.verify_record(size);
        const auto verified_at = clock_type::now();
        if (latency_samples != nullptr) {
            latency_samples->push_back(
                elapsed_ns(sample_begin, verified_at));
        }
        if (sequence + 1 == count && total_elapsed != nullptr) {
            *total_elapsed = elapsed_ns(phase_begin, verified_at);
        }
        if (sequence + 1 == count && process_cpu_elapsed != nullptr) {
            const uint64_t process_cpu_end = bench::process_cpu_time_ns();
            *process_cpu_elapsed = process_cpu_end >= process_cpu_begin
                ? process_cpu_end - process_cpu_begin : 0;
        }
    }
    return true;
}

bool run_pipelined_phase(int socket, bench::phase phase, uint64_t trial,
                         uint64_t count, std::size_t size,
                         uint32_t credit_window, bench::counters& counters,
                         uint64_t* total_elapsed,
                         uint64_t* process_cpu_elapsed) {
    std::mutex mutex;
    std::condition_variable credit_changed;
    uint64_t submitted = 0;
    uint64_t received = 0;
    enum class failure { none, transport, payload };
    std::atomic<failure> failed{failure::none};
    std::barrier start_line(3);
    clock_type::time_point begin;
    clock_type::time_point end;
    uint64_t process_cpu_begin = 0;
    uint64_t process_cpu_end = 0;
    const auto mark_failed = [&](failure reason) {
        failure expected = failure::none;
        (void)failed.compare_exchange_strong(
            expected, reason, std::memory_order_acq_rel);
        ::shutdown(socket, SHUT_RDWR);
        credit_changed.notify_all();
    };

    std::thread reader([&] {
        std::vector<uint8_t> incoming(size);
        start_line.arrive_and_wait();
        for (uint64_t sequence = 0; sequence != count; ++sequence) {
            if (!receive_all(socket, incoming.data(), incoming.size())) {
                mark_failed(failure::transport);
                return;
            }
            counters.receive_record(size);
            if (bench::validate_record(incoming, phase, trial, sequence, size) !=
                bench::record_error::none) {
                mark_failed(failure::payload);
                return;
            }
            counters.verify_record(size);
            if (sequence + 1 == count) {
                end = clock_type::now();
                if (process_cpu_elapsed != nullptr) {
                    process_cpu_end = bench::process_cpu_time_ns();
                }
            }
            {
                std::lock_guard lock(mutex);
                ++received;
            }
            credit_changed.notify_one();
        }
    });

    std::thread writer([&] {
        std::vector<uint8_t> outgoing(size);
        if (!bench::initialize_record(outgoing)) {
            failed.store(failure::payload, std::memory_order_release);
            ::shutdown(socket, SHUT_RDWR);
            credit_changed.notify_all();
            start_line.arrive_and_wait();
            return;
        }
        start_line.arrive_and_wait();
        for (uint64_t sequence = 0; sequence != count; ++sequence) {
            {
                std::unique_lock lock(mutex);
                credit_changed.wait(lock, [&] {
                    return failed.load(std::memory_order_acquire) !=
                               failure::none ||
                           submitted - received < credit_window;
                });
                if (failed.load(std::memory_order_acquire) != failure::none) {
                    return;
                }
                ++submitted;
                counters.observe_unacknowledged(submitted - received);
            }
            if (!bench::stamp_record(outgoing, phase, trial, sequence)) {
                mark_failed(failure::payload);
                return;
            }
            if (sequence == 0) {
                begin = clock_type::now();
                if (process_cpu_elapsed != nullptr) {
                    process_cpu_begin = bench::process_cpu_time_ns();
                }
            }
            counters.begin_write(size, 1, size);
            if (!send_all(socket, outgoing.data(), outgoing.size())) {
                mark_failed(failure::transport);
                return;
            }
            counters.complete_write(size, 1, size);
        }
    });

    start_line.arrive_and_wait();
    writer.join();
    reader.join();
    const auto failure_reason = failed.load(std::memory_order_acquire);
    if (failure_reason == failure::transport) {
        ++counters.transport_errors;
    } else if (failure_reason == failure::payload) {
        ++counters.payload_errors;
    }
    if (total_elapsed != nullptr && failure_reason == failure::none) {
        *total_elapsed = elapsed_ns(begin, end);
    }
    if (process_cpu_elapsed != nullptr && failure_reason == failure::none) {
        *process_cpu_elapsed = process_cpu_end >= process_cpu_begin
            ? process_cpu_end - process_cpu_begin : 0;
    }
    return failure_reason == failure::none;
}

bench::result run_one(const bench::config& cfg, bench::workload workload,
                      std::size_t size) {
    bench::result result;
    result.implementation = cfg.peer_implementation;
    result.role = "server";
    result.peer = "posix-reference-client";
    result.counter_scope = "driver";
    result.workload_type = workload;
    result.trial = bench::selected_trial(cfg, workload, size);
    result.write_size = size;
    result.credit_window = workload == bench::workload::latency
        ? 1 : cfg.credit_window;
    result.warmup_expected_records = cfg.warmup_records;
    result.measured_expected_records = workload == bench::workload::bulk
        ? cfg.bulk_bytes / cfg.chunk_bytes : cfg.records;
    if (workload == bench::workload::latency) {
        result.latency_samples_ns.reserve(result.measured_expected_records);
    }

    const int socket = connect_peer(cfg);
    if (socket < 0) {
        ++result.warmup.transport_errors;
        return result;
    }
    const auto run_phase = [&](bench::phase phase, uint64_t count,
                               bench::counters& counters,
                               uint64_t* elapsed,
                               uint64_t* process_cpu_elapsed) {
        if (workload == bench::workload::latency) {
            auto* samples = phase == bench::phase::measured
                ? &result.latency_samples_ns : nullptr;
            return run_sequential_phase(socket, phase, result.trial, count,
                                        size, counters, samples, elapsed,
                                        process_cpu_elapsed);
        }
        return run_pipelined_phase(socket, phase, result.trial, count, size,
                                   result.credit_window, counters, elapsed,
                                   process_cpu_elapsed);
    };

    if (run_phase(bench::phase::warmup, result.warmup_expected_records,
                  result.warmup, nullptr, nullptr)) {
        result.warmup_drained = true;
        (void)run_phase(bench::phase::measured,
                        result.measured_expected_records, result.measured,
                        &result.elapsed_ns,
                        &result.process_cpu_elapsed_ns);
    }
    ::close(socket);
    return result;
}

bool emit(const bench::config& cfg, const bench::result& result) {
    const auto json = bench::to_json_line(result);
    std::printf("%s\n", json.c_str());
    bench::print_human(result);
    std::string error;
    if (!bench::append_jsonl(cfg.json_path, result, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    return result.valid();
}

} // namespace

int main(int argc, char* argv[]) {
    bench::config cfg;
    try {
        cfg = bench::parse_args(argc, argv, "POSIX reference client");
    } catch (const bench::argument_error&) {
        return 2;
    }
    if (cfg.peer_implementation.empty()) {
        std::fprintf(stderr, "--peer-implementation is required\n");
        return 2;
    }
    std::ofstream(cfg.json_path, std::ios::trunc).close();

    bool valid = true;
    const auto sizes = bench::selected_message_sizes(cfg);
    if (cfg.type == bench::workload::latency ||
        cfg.type == bench::workload::all) {
        for (const auto size : sizes) {
            valid = emit(cfg, run_one(cfg, bench::workload::latency, size)) &&
                    valid;
        }
    }
    if (cfg.type == bench::workload::message ||
        cfg.type == bench::workload::all) {
        for (const auto size : sizes) {
            valid = emit(cfg, run_one(cfg, bench::workload::message, size)) &&
                    valid;
        }
    }
    if (cfg.type == bench::workload::bulk ||
        cfg.type == bench::workload::all) {
        valid = emit(cfg, run_one(cfg, bench::workload::bulk,
                                  cfg.chunk_bytes)) && valid;
    }
    return valid ? 0 : 1;
}
