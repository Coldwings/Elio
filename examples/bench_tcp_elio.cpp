/// @file bench_tcp_elio.cpp
/// @brief Fair TCP loopback client adapter using Elio coroutines.

#include "bench_tcp_common.hpp"

#include <elio/elio.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <vector>

using namespace elio;
using namespace elio::coro;
using namespace elio::net;
using namespace elio::runtime;

namespace {

task<bool> read_exact(tcp_stream& stream, std::span<uint8_t> bytes,
                      cancel_token token) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto operation = co_await stream.read(
            bytes.data() + offset, bytes.size() - offset, token);
        if (operation.result <= 0) co_return false;
        offset += static_cast<std::size_t>(operation.result);
    }
    co_return true;
}

task<bool> write_exact(tcp_stream& stream, std::span<const uint8_t> bytes,
                       cancel_token token) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto operation = co_await stream.write(
            bytes.data() + offset, bytes.size() - offset, token);
        if (operation.result <= 0) co_return false;
        offset += static_cast<std::size_t>(operation.result);
    }
    co_return true;
}

struct phase_context {
    tcp_stream& stream;
    bench::phase phase;
    uint64_t trial;
    uint64_t target;
    std::size_t write_size;
    uint32_t credit_window;
    bool collect_latency;
    bench::counters& counters;
    sync::semaphore credits;
    cancel_source cancel;
    std::vector<uint8_t> send;
    std::vector<uint8_t> receive;
    std::vector<std::chrono::steady_clock::time_point> send_times;
    std::vector<uint64_t>* latency_samples;
    std::chrono::steady_clock::time_point* phase_start;
    std::chrono::steady_clock::time_point* phase_end;
    uint64_t unacknowledged = 0;
    bool failed = false;

    phase_context(tcp_stream& stream_value, bench::phase phase_value,
                  uint64_t trial_value, uint64_t target_value,
                  std::size_t write_size_value, uint32_t credit_window_value,
                  bool collect_latency_value, bench::counters& counters_value,
                  std::vector<uint64_t>* latency_samples_value,
                  std::chrono::steady_clock::time_point* phase_start_value,
                  std::chrono::steady_clock::time_point* phase_end_value)
        : stream(stream_value), phase(phase_value), trial(trial_value),
          target(target_value), write_size(write_size_value),
          credit_window(credit_window_value),
          collect_latency(collect_latency_value), counters(counters_value),
          credits(static_cast<int>(credit_window_value)), send(write_size_value),
          receive(write_size_value),
          send_times(collect_latency_value ? target_value : 0),
          latency_samples(latency_samples_value),
          phase_start(phase_start_value), phase_end(phase_end_value) {
        if (!bench::initialize_record(std::span<uint8_t>(send))) {
            throw std::invalid_argument("invalid TCP benchmark record size");
        }
    }

    void fail_transport() {
        if (!failed) counters.transport_errors++;
        failed = true;
        cancel.cancel();
        stream.shutdown_socket();
    }
};

task<void> phase_writer(phase_context& ctx) {
    try {
        const auto token = ctx.cancel.get_token();
        for (uint64_t sequence = 0; sequence < ctx.target; ++sequence) {
            if (co_await ctx.credits.acquire(token) !=
                cancel_result::completed) {
                co_return;
            }
            if (!bench::stamp_record(std::span<uint8_t>(ctx.send), ctx.phase,
                                     ctx.trial, sequence)) {
                ctx.counters.payload_errors++;
                ctx.failed = true;
                ctx.cancel.cancel();
                ctx.stream.shutdown_socket();
                co_return;
            }
            if (ctx.collect_latency) {
                ctx.send_times[sequence] = std::chrono::steady_clock::now();
            }
            if (sequence == 0 && ctx.phase_start != nullptr) {
                *ctx.phase_start = std::chrono::steady_clock::now();
            }
            ++ctx.unacknowledged;
            ctx.counters.observe_unacknowledged(ctx.unacknowledged);
            ctx.counters.begin_write(ctx.write_size, 1, ctx.write_size);
            if (!co_await write_exact(
                    ctx.stream, std::span<const uint8_t>(ctx.send), token)) {
                ctx.fail_transport();
                co_return;
            }
            ctx.counters.complete_write(ctx.write_size, 1, ctx.write_size);
        }
    } catch (...) {
        ctx.fail_transport();
    }
}

task<void> phase_reader(phase_context& ctx) {
    try {
        const auto token = ctx.cancel.get_token();
        for (uint64_t sequence = 0; sequence < ctx.target; ++sequence) {
            if (!co_await read_exact(ctx.stream,
                                     std::span<uint8_t>(ctx.receive), token)) {
                ctx.fail_transport();
                co_return;
            }
            ctx.counters.receive_record(ctx.write_size);
            const auto validation = bench::validate_record(
                std::span<const uint8_t>(ctx.receive), ctx.phase, ctx.trial,
                sequence, ctx.write_size);
            if (validation != bench::record_error::none) {
                if (validation == bench::record_error::sequence) {
                    ctx.counters.sequence_errors++;
                } else if (validation == bench::record_error::phase) {
                    ctx.counters.phase_errors++;
                } else {
                    ctx.counters.payload_errors++;
                }
                ctx.failed = true;
                ctx.cancel.cancel();
                ctx.stream.shutdown_socket();
                co_return;
            }
            ctx.counters.verify_record(ctx.write_size);
            if (ctx.unacknowledged == 0) {
                ctx.counters.sequence_errors++;
                ctx.failed = true;
                ctx.cancel.cancel();
                ctx.stream.shutdown_socket();
                co_return;
            }
            --ctx.unacknowledged;
            if (sequence + 1 == ctx.target && ctx.phase_end != nullptr) {
                *ctx.phase_end = std::chrono::steady_clock::now();
            }
            if (ctx.collect_latency && ctx.latency_samples != nullptr) {
                ctx.latency_samples->push_back(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() -
                        ctx.send_times[sequence])
                        .count()));
            }
            ctx.credits.release();
        }
    } catch (...) {
        ctx.fail_transport();
    }
}

task<bool> run_phase(tcp_stream& stream, bench::phase phase, uint64_t trial,
                     uint64_t target, std::size_t write_size,
                     uint32_t credit_window, bool collect_latency,
                     bench::counters& counters,
                     std::vector<uint64_t>* latency_samples,
                     std::chrono::steady_clock::time_point* phase_start,
                     std::chrono::steady_clock::time_point* phase_end) {
    if (target == 0) co_return true;
    phase_context context(stream, phase, trial, target, write_size,
                          credit_window, collect_latency, counters,
                          latency_samples, phase_start, phase_end);
    auto* scheduler = get_current_scheduler();
    try {
        auto reader = scheduler->go_joinable(phase_reader, std::ref(context));
        try {
            auto writer = scheduler->go_joinable(phase_writer,
                                                 std::ref(context));
            co_await writer;
        } catch (...) {
            context.fail_transport();
        }
        co_await reader;
    } catch (...) {
        context.fail_transport();
    }
    co_return !context.failed;
}

task<bench::result> run_one(const bench::config& cfg,
                            bench::workload workload,
                            std::size_t write_size, uint64_t trial) {
    bench::result result;
    result.implementation = "elio";
    if (!cfg.peer_implementation.empty()) {
        result.peer = cfg.peer_implementation;
    }
    result.workload_type = workload;
    result.trial = trial;
    result.write_size = write_size;
    result.credit_window = workload == bench::workload::latency
                               ? 1U
                               : cfg.credit_window;
    result.warmup_expected_records = cfg.warmup_records;
    result.measured_expected_records =
        workload == bench::workload::bulk
            ? cfg.bulk_bytes / cfg.chunk_bytes
            : cfg.records;
    if (workload == bench::workload::latency) {
        result.latency_samples_ns.reserve(result.measured_expected_records);
    }

    auto resolved = co_await resolve_hostname(cfg.host, cfg.port);
    if (!resolved) {
        result.warmup.transport_errors++;
        co_return result;
    }
    auto stream = co_await tcp_connect(*resolved);
    if (!stream) {
        result.warmup.transport_errors++;
        co_return result;
    }
    stream->set_no_delay(true);

    const bool latency = workload == bench::workload::latency;
    if (!co_await run_phase(*stream, bench::phase::warmup, trial,
                            result.warmup_expected_records, write_size,
                            result.credit_window, false, result.warmup,
                            nullptr, nullptr, nullptr)) {
        co_return result;
    }
    result.warmup_drained = true;

    std::chrono::steady_clock::time_point start;
    std::chrono::steady_clock::time_point end;
    if (!co_await run_phase(*stream, bench::phase::measured, trial,
                            result.measured_expected_records, write_size,
                            result.credit_window, latency, result.measured,
                            &result.latency_samples_ns, &start, &end)) {
        co_return result;
    }
    result.elapsed_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - start)
            .count());
    co_return result;
}

bool selected(const bench::config& cfg, bench::workload value) {
    return cfg.type == bench::workload::all || cfg.type == value;
}

bool publish(const bench::config& cfg, const bench::result& result) {
    bench::print_human(result);
    const std::string json = bench::to_json_line(result);
    std::printf("%s\n", json.c_str());
    std::string error;
    if (!bench::append_jsonl(cfg.json_path, result, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    return result.valid();
}

task<void> client_main(const bench::config& cfg, std::atomic<bool>& ok) {
    if (selected(cfg, bench::workload::latency)) {
        for (std::size_t size : bench::selected_message_sizes(cfg)) {
            ok.store(publish(cfg, co_await run_one(
                                      cfg, bench::workload::latency, size,
                                      bench::selected_trial(
                                          cfg, bench::workload::latency,
                                          size))) &&
                         ok.load(std::memory_order_relaxed),
                     std::memory_order_relaxed);
        }
    }
    if (selected(cfg, bench::workload::message)) {
        for (std::size_t size : bench::selected_message_sizes(cfg)) {
            ok.store(publish(cfg, co_await run_one(
                                      cfg, bench::workload::message, size,
                                      bench::selected_trial(
                                          cfg, bench::workload::message,
                                          size))) &&
                         ok.load(std::memory_order_relaxed),
                     std::memory_order_relaxed);
        }
    }
    if (selected(cfg, bench::workload::bulk)) {
        ok.store(publish(cfg, co_await run_one(
                                  cfg, bench::workload::bulk, cfg.chunk_bytes,
                                  bench::selected_trial(
                                      cfg, bench::workload::bulk,
                                      cfg.chunk_bytes))) &&
                     ok.load(std::memory_order_relaxed),
                 std::memory_order_relaxed);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    elio::log::logger::instance().set_level(elio::log::level::error);
    bench::config cfg;
    try {
        cfg = bench::parse_args(argc, argv, "Elio");
    } catch (const bench::argument_error&) {
        return 2;
    }

    std::ofstream(cfg.json_path, std::ios::trunc).close();
    scheduler scheduler(1);
    scheduler.start();
    std::atomic<bool> ok{true};
    try {
        auto completion = scheduler.go_joinable([&]() -> task<void> {
            co_await client_main(cfg, ok);
        });
        completion.wait_destroyed();
        completion.await_resume();
    } catch (...) {
        ok.store(false, std::memory_order_relaxed);
    }
    scheduler.shutdown();
    return ok.load(std::memory_order_relaxed) ? 0 : 1;
}
