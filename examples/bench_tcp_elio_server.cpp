/// @file bench_tcp_elio_server.cpp
/// @brief Protocol-validating Elio server for TCP benchmark attribution.

#include <elio/elio.hpp>

#include "bench_tcp_server_protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <span>
#include <thread>
#include <vector>

namespace {

using elio::coro::task;
using elio::net::ipv4_address;
using elio::net::tcp_listener;
using elio::net::tcp_options;
using elio::net::tcp_stream;
using elio::runtime::scheduler;

task<void> serve_connection(tcp_stream stream, std::size_t maximum_record_size) {
    std::vector<uint8_t> record(maximum_record_size);
    bench::server_protocol protocol;
    bench::server_connection_evidence evidence("elio");

    while (true) {
        auto header_read = co_await stream.read_exactly(
            record.data(), bench::kRecordHeaderBytes);
        if (header_read.result !=
            static_cast<int32_t>(bench::kRecordHeaderBytes)) {
            co_return;
        }

        const auto header = bench::decode_record_header(
            std::span<const uint8_t>(record.data(), bench::kRecordHeaderBytes));
        if (!bench::valid_record_size(header.size, maximum_record_size)) {
            evidence.integrity_error();
            co_return;
        }
        const std::size_t remainder = header.size - bench::kRecordHeaderBytes;
        if (remainder != 0) {
            auto body_read = co_await stream.read_exactly(
                record.data() + bench::kRecordHeaderBytes, remainder);
            if (body_read.result != static_cast<int32_t>(remainder)) {
                evidence.transport_error();
                co_return;
            }
        }

        const auto bytes = std::span<const uint8_t>(record.data(), header.size);
        evidence.receive_record(bytes);
        if (!protocol.accept(bytes)) {
            evidence.integrity_error();
            co_return;
        }
        evidence.verify_record(bytes.size());
        evidence.begin_write(bytes.size());
        auto write = co_await stream.write_exactly(bytes);
        if (write.result != static_cast<int32_t>(bytes.size())) {
            evidence.transport_error();
            co_return;
        }
        evidence.complete_write(bytes.size());
    }
}

task<void> accept_loop(const bench::config& cfg, scheduler& sched,
                       std::atomic<bool>& failed) {
    tcp_options options;
    options.no_delay = true;
    options.reuse_addr = true;
    auto listener = tcp_listener::bind(
        ipv4_address("127.0.0.1", cfg.port), options);
    if (!listener) {
        std::fprintf(stderr, "Elio benchmark server failed to bind port %u\n",
                     static_cast<unsigned>(cfg.port));
        failed.store(true, std::memory_order_release);
        co_return;
    }

    std::printf("Elio benchmark server listening on 127.0.0.1:%u\n",
                static_cast<unsigned>(cfg.port));
    std::fflush(stdout);
    const auto maximum = bench::server_max_record_size(cfg);
    while (true) {
        auto stream = co_await listener->accept();
        if (!stream) {
            continue;
        }
        stream->set_no_delay(true);
        sched.go([connection = std::move(*stream), maximum]() mutable {
            return serve_connection(std::move(connection), maximum);
        });
    }
}

} // namespace

int main(int argc, char* argv[]) {
    bench::config cfg;
    try {
        cfg = bench::parse_args(argc, argv, "Elio server");
    } catch (const bench::argument_error&) {
        return 2;
    }

    elio::log::logger::instance().set_level(elio::log::level::error);
    scheduler sched(1);
    sched.start();
    std::atomic<bool> failed{false};
    sched.go([&]() { return accept_loop(cfg, sched, failed); });

    while (!failed.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    sched.shutdown();
    return 1;
}
