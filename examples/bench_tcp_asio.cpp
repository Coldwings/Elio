/// @file bench_tcp_asio.cpp
/// @brief Fair TCP loopback client adapter using standalone Asio.

#include "bench_tcp_common.hpp"

#include <asio.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

using asio::ip::tcp;

namespace {

class fixed_work_session {
public:
    fixed_work_session(const bench::config& cfg, bench::workload workload,
                       std::size_t write_size, uint64_t trial)
        : cfg_(cfg), io_(), socket_(io_), workload_(workload),
          write_size_(write_size), trial_(trial), send_(write_size),
          receive_(write_size) {
        if (!bench::initialize_record(std::span<uint8_t>(send_))) {
            throw std::invalid_argument("invalid TCP benchmark record size");
        }
        result_.implementation = "asio";
        if (!cfg.peer_implementation.empty()) {
            result_.peer = cfg.peer_implementation;
        }
        result_.workload_type = workload;
        result_.trial = trial;
        result_.write_size = write_size;
        result_.credit_window = workload == bench::workload::latency
                                    ? 1U
                                    : cfg.credit_window;
        result_.warmup_expected_records = cfg.warmup_records;
        result_.measured_expected_records =
            workload == bench::workload::bulk
                ? cfg.bulk_bytes / cfg.chunk_bytes
                : cfg.records;
        if (workload == bench::workload::latency) {
            result_.latency_samples_ns.reserve(
                result_.measured_expected_records);
        }
    }

    bench::result run() {
        tcp::resolver resolver(io_);
        std::error_code error;
        const auto endpoints = resolver.resolve(
            cfg_.host, std::to_string(cfg_.port), error);
        if (!error) asio::connect(socket_, endpoints, error);
        if (error) {
            result_.warmup.transport_errors++;
            return result_;
        }
        socket_.set_option(tcp::no_delay(true), error);
        if (error) {
            result_.warmup.transport_errors++;
            return result_;
        }

        start_phase(bench::phase::warmup);
        io_.run();
        return result_;
    }

private:
    bench::counters& current_counters() noexcept {
        return phase_ == bench::phase::warmup ? result_.warmup
                                               : result_.measured;
    }

    uint64_t target() const noexcept {
        return phase_ == bench::phase::warmup
                   ? result_.warmup_expected_records
                   : result_.measured_expected_records;
    }

    void start_phase(bench::phase next) {
        phase_ = next;
        next_write_ = 0;
        next_read_ = 0;
        unacknowledged_ = 0;
        if (target() == 0) {
            finish_phase();
            return;
        }
        start_read();
        maybe_start_write();
    }

    void start_read() {
        if (done_ || read_active_ || next_read_ >= target()) return;
        read_active_ = true;
        asio::async_read(
            socket_, asio::buffer(receive_),
            [this](std::error_code error, std::size_t bytes) {
                read_active_ = false;
                auto& counters = current_counters();
                if (error || bytes != write_size_) {
                    counters.transport_errors++;
                    fail();
                    return;
                }
                counters.receive_record(bytes);
                const auto validation = bench::validate_record(
                    std::span<const uint8_t>(receive_), phase_, trial_,
                    next_read_, write_size_);
                if (validation != bench::record_error::none) {
                    if (validation == bench::record_error::sequence) {
                        counters.sequence_errors++;
                    } else if (validation == bench::record_error::phase) {
                        counters.phase_errors++;
                    } else {
                        counters.payload_errors++;
                    }
                    fail();
                    return;
                }
                counters.verify_record(bytes);
                if (unacknowledged_ == 0) {
                    counters.sequence_errors++;
                    fail();
                    return;
                }
                --unacknowledged_;
                ++next_read_;
                if (phase_ == bench::phase::measured &&
                    next_read_ == target()) {
                    measure_end_ = std::chrono::steady_clock::now();
                }
                if (phase_ == bench::phase::measured &&
                    workload_ == bench::workload::latency) {
                    result_.latency_samples_ns.push_back(
                        static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - latency_start_)
                                .count()));
                }
                if (phase_complete()) {
                    finish_phase();
                    return;
                }
                start_read();
                maybe_start_write();
            });
    }

    void maybe_start_write() {
        if (done_ || write_active_ || next_write_ >= target() ||
            unacknowledged_ >= result_.credit_window) {
            return;
        }

        const uint64_t sequence = next_write_++;
        if (!bench::stamp_record(std::span<uint8_t>(send_), phase_, trial_,
                                 sequence)) {
            current_counters().payload_errors++;
            fail();
            return;
        }
        if (phase_ == bench::phase::measured &&
            workload_ == bench::workload::latency) {
            latency_start_ = std::chrono::steady_clock::now();
        }
        if (phase_ == bench::phase::measured && sequence == 0) {
            measure_start_ = std::chrono::steady_clock::now();
        }

        ++unacknowledged_;
        auto& counters = current_counters();
        counters.observe_unacknowledged(unacknowledged_);
        counters.begin_write(write_size_, 1, write_size_);
        write_active_ = true;
        asio::async_write(
            socket_, asio::buffer(send_),
            [this](std::error_code error, std::size_t bytes) {
                write_active_ = false;
                auto& counters = current_counters();
                if (error || bytes != write_size_) {
                    counters.transport_errors++;
                    fail();
                    return;
                }
                counters.complete_write(bytes, 1, write_size_);
                if (phase_complete()) {
                    finish_phase();
                    return;
                }
                maybe_start_write();
            });
    }

    bool phase_complete() const noexcept {
        const auto& counters = phase_ == bench::phase::warmup
                                   ? result_.warmup
                                   : result_.measured;
        return next_read_ == target() && next_write_ == target() &&
               counters.write_completions == target() &&
               unacknowledged_ == 0 && !write_active_ && !read_active_;
    }

    void finish_phase() {
        if (phase_ == bench::phase::warmup) {
            result_.warmup_drained = true;
            start_phase(bench::phase::measured);
            return;
        }
        result_.elapsed_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                measure_end_ - measure_start_)
                .count());
        done_ = true;
        std::error_code ignored;
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
    }

    void fail() {
        done_ = true;
        std::error_code ignored;
        socket_.cancel(ignored);
        socket_.close(ignored);
    }

    const bench::config& cfg_;
    asio::io_context io_;
    tcp::socket socket_;
    bench::workload workload_;
    std::size_t write_size_;
    uint64_t trial_;
    bench::phase phase_ = bench::phase::warmup;
    std::vector<uint8_t> send_;
    std::vector<uint8_t> receive_;
    uint64_t next_write_ = 0;
    uint64_t next_read_ = 0;
    uint64_t unacknowledged_ = 0;
    bool write_active_ = false;
    bool read_active_ = false;
    bool done_ = false;
    std::chrono::steady_clock::time_point measure_start_{};
    std::chrono::steady_clock::time_point measure_end_{};
    std::chrono::steady_clock::time_point latency_start_{};
    bench::result result_;
};

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

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    bench::config cfg;
    try {
        cfg = bench::parse_args(argc, argv, "Asio");
    } catch (const bench::argument_error&) {
        return 2;
    }

    std::ofstream(cfg.json_path, std::ios::trunc).close();
    bool ok = true;
    auto run = [&](bench::workload workload, std::size_t size) {
        fixed_work_session session(
            cfg, workload, size, bench::selected_trial(cfg, workload, size));
        ok = publish(cfg, session.run()) && ok;
    };

    if (selected(cfg, bench::workload::latency)) {
        for (std::size_t size : bench::selected_message_sizes(cfg)) {
            run(bench::workload::latency, size);
        }
    }
    if (selected(cfg, bench::workload::message)) {
        for (std::size_t size : bench::selected_message_sizes(cfg)) {
            run(bench::workload::message, size);
        }
    }
    if (selected(cfg, bench::workload::bulk)) {
        run(bench::workload::bulk, cfg.chunk_bytes);
    }
    return ok ? 0 : 1;
}
