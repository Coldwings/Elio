/// @file bench_tcp_libuv.cpp
/// @brief Fair TCP loopback client adapter using libuv.

#include "bench_tcp_common.hpp"

#include <uv.h>

#include <netdb.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace {

class fixed_work_session {
public:
    fixed_work_session(const bench::config& cfg, bench::workload workload,
                       std::size_t write_size, uint64_t trial)
        : cfg_(cfg), workload_(workload), write_size_(write_size),
          trial_(trial), send_(write_size), receive_(write_size) {
        if (!bench::initialize_record(std::span<uint8_t>(send_))) {
            throw std::invalid_argument("invalid TCP benchmark record size");
        }
        result_.implementation = "libuv";
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
        if (uv_loop_init(&loop_) != 0) {
            result_.warmup.transport_errors++;
            return result_;
        }
        uv_tcp_init(&loop_, &tcp_);
        tcp_.data = this;
        connect_.data = this;
        write_.data = this;

        sockaddr_in address{};
        if (!resolve(address)) {
            result_.warmup.transport_errors++;
            uv_close(reinterpret_cast<uv_handle_t*>(&tcp_), nullptr);
        } else {
            const int rc = uv_tcp_connect(
                &connect_, &tcp_,
                reinterpret_cast<const sockaddr*>(&address), connect_callback);
            if (rc != 0) {
                result_.warmup.transport_errors++;
                uv_close(reinterpret_cast<uv_handle_t*>(&tcp_), nullptr);
            }
        }
        uv_run(&loop_, UV_RUN_DEFAULT);
        uv_loop_close(&loop_);
        return result_;
    }

private:
    bool resolve(sockaddr_in& address) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* resolved = nullptr;
        const std::string service = std::to_string(cfg_.port);
        const int rc = getaddrinfo(cfg_.host.c_str(), service.c_str(), &hints,
                                   &resolved);
        if (rc != 0 || resolved == nullptr) return false;
        std::memcpy(&address, resolved->ai_addr, sizeof(address));
        freeaddrinfo(resolved);
        return true;
    }

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
        receive_offset_ = 0;
        unacknowledged_ = 0;
        if (target() == 0) {
            finish_phase();
            return;
        }
        maybe_start_write();
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
        uv_buf_t buffer = uv_buf_init(
            reinterpret_cast<char*>(send_.data()),
            static_cast<unsigned int>(send_.size()));
        const int rc = uv_write(&write_, reinterpret_cast<uv_stream_t*>(&tcp_),
                                &buffer, 1, write_callback);
        if (rc != 0) {
            write_active_ = false;
            counters.transport_errors++;
            fail();
        }
    }

    void on_write(int status) {
        write_active_ = false;
        auto& counters = current_counters();
        if (status != 0) {
            counters.transport_errors++;
            fail();
            return;
        }
        counters.complete_write(write_size_, 1, write_size_);
        if (phase_complete()) {
            finish_phase();
            return;
        }
        maybe_start_write();
    }

    void on_read(ssize_t bytes) {
        if (bytes == 0 || done_) return;
        auto& counters = current_counters();
        if (bytes < 0) {
            counters.transport_errors++;
            fail();
            return;
        }
        receive_offset_ += static_cast<std::size_t>(bytes);
        if (receive_offset_ < write_size_) return;
        if (receive_offset_ != write_size_) {
            counters.transport_errors++;
            fail();
            return;
        }

        counters.receive_record(write_size_);
        const auto validation = bench::validate_record(
            std::span<const uint8_t>(receive_), phase_, trial_, next_read_,
            write_size_);
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
        counters.verify_record(write_size_);
        if (unacknowledged_ == 0) {
            counters.sequence_errors++;
            fail();
            return;
        }
        --unacknowledged_;
        ++next_read_;
        if (phase_ == bench::phase::measured && next_read_ == target()) {
            measure_end_ = std::chrono::steady_clock::now();
        }
        receive_offset_ = 0;
        if (phase_ == bench::phase::measured &&
            workload_ == bench::workload::latency) {
            result_.latency_samples_ns.push_back(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - latency_start_)
                    .count()));
        }
        if (phase_complete()) {
            finish_phase();
            return;
        }
        maybe_start_write();
    }

    bool phase_complete() const noexcept {
        const auto& counters = phase_ == bench::phase::warmup
                                   ? result_.warmup
                                   : result_.measured;
        return next_read_ == target() && next_write_ == target() &&
               counters.write_completions == target() &&
               unacknowledged_ == 0 && !write_active_;
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
        close();
    }

    void fail() { close(); }

    void close() {
        if (done_) return;
        done_ = true;
        uv_read_stop(reinterpret_cast<uv_stream_t*>(&tcp_));
        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&tcp_))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&tcp_), nullptr);
        }
    }

    static void connect_callback(uv_connect_t* request, int status) {
        auto* self = static_cast<fixed_work_session*>(request->data);
        if (status != 0) {
            self->result_.warmup.transport_errors++;
            self->close();
            return;
        }
        uv_tcp_nodelay(&self->tcp_, 1);
        const int rc = uv_read_start(
            reinterpret_cast<uv_stream_t*>(&self->tcp_), alloc_callback,
            read_callback);
        if (rc != 0) {
            self->result_.warmup.transport_errors++;
            self->close();
            return;
        }
        self->start_phase(bench::phase::warmup);
    }

    static void alloc_callback(uv_handle_t* handle, std::size_t,
                               uv_buf_t* buffer) {
        auto* self = static_cast<fixed_work_session*>(handle->data);
        buffer->base = reinterpret_cast<char*>(self->receive_.data() +
                                               self->receive_offset_);
        buffer->len = static_cast<unsigned int>(self->write_size_ -
                                                self->receive_offset_);
    }

    static void read_callback(uv_stream_t* stream, ssize_t bytes,
                              const uv_buf_t*) {
        static_cast<fixed_work_session*>(stream->data)->on_read(bytes);
    }

    static void write_callback(uv_write_t* request, int status) {
        static_cast<fixed_work_session*>(request->data)->on_write(status);
    }

    const bench::config& cfg_;
    bench::workload workload_;
    std::size_t write_size_;
    uint64_t trial_;
    uv_loop_t loop_{};
    uv_tcp_t tcp_{};
    uv_connect_t connect_{};
    uv_write_t write_{};
    bench::phase phase_ = bench::phase::warmup;
    std::vector<uint8_t> send_;
    std::vector<uint8_t> receive_;
    std::size_t receive_offset_ = 0;
    uint64_t next_write_ = 0;
    uint64_t next_read_ = 0;
    uint64_t unacknowledged_ = 0;
    bool write_active_ = false;
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
        cfg = bench::parse_args(argc, argv, "libuv");
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
