/// @file bench_tcp_server_protocol.hpp
/// @brief Shared record validation for TCP benchmark server adapters.
#pragma once

#include "bench_tcp_common.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>

namespace bench {

/// Validates the application-record stream independently from TCP read
/// boundaries. A connection may carry several trials. Each trial starts with
/// warmup sequence zero, then transitions to measured sequence zero.
class server_protocol {
public:
    bool accept(std::span<const uint8_t> record, std::string* error = nullptr) {
        const auto header = decode_record_header(record);
        if (header.record_phase != phase::warmup &&
            header.record_phase != phase::measured) {
            return fail("invalid phase", error);
        }

        if (!active_ || header.trial != trial_) {
            if (header.sequence != 0) {
                return fail("a trial must start at sequence zero", error);
            }
            active_ = true;
            trial_ = header.trial;
            phase_ = header.record_phase;
            next_sequence_ = 0;
        } else if (header.record_phase != phase_) {
            if (phase_ != phase::warmup ||
                header.record_phase != phase::measured ||
                header.sequence != 0) {
                return fail("invalid phase transition", error);
            }
            phase_ = phase::measured;
            next_sequence_ = 0;
        }

        const auto validation = validate_record(
            record, phase_, trial_, next_sequence_, record.size());
        if (validation != record_error::none) {
            return fail("record integrity validation failed", error);
        }
        ++next_sequence_;
        return true;
    }

private:
    static bool fail(const char* message, std::string* error) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    }

    bool active_ = false;
    uint64_t trial_ = 0;
    phase phase_ = phase::warmup;
    uint64_t next_sequence_ = 0;
};

/// Per-connection evidence emitted by the server adapter itself. Server-side
/// performance is timed by the common reference client; these counters exist
/// separately so the runner never mistakes driver activity for server write
/// concurrency or integrity evidence.
class server_connection_evidence {
public:
    explicit server_connection_evidence(const char* implementation)
        : implementation_(implementation),
          connection_cpu_start_ns_(process_cpu_time_ns()) {}

    ~server_connection_evidence() { emit(); }

    void receive_record(std::span<const uint8_t> record) noexcept {
        active_ = true;
        const auto header = decode_record_header(record);
        if (write_size_ == 0) {
            write_size_ = record.size();
            trial_ = header.trial;
        } else if (write_size_ != record.size() || trial_ != header.trial) {
            ++accounting_errors_;
        }
        if (header.record_phase == phase::warmup) {
            ++warmup_records_;
            warmup_bytes_ += record.size();
        } else if (header.record_phase == phase::measured) {
            if (!measured_cpu_started_) {
                measured_cpu_started_ = true;
                measured_cpu_start_ns_ = process_cpu_time_ns();
            }
            ++measured_records_;
            measured_bytes_ += record.size();
        } else {
            ++integrity_errors_;
        }
        ++received_records_;
        received_bytes_ += record.size();
    }

    void verify_record(std::size_t bytes) noexcept {
        ++verified_records_;
        verified_bytes_ += bytes;
    }

    void begin_write(std::size_t bytes) noexcept {
        ++write_submissions_;
        ++current_writes_;
        max_writes_ = std::max(max_writes_, current_writes_);
        submitted_bytes_ += bytes;
    }

    void complete_write(std::size_t bytes) noexcept {
        ++write_completions_;
        completed_bytes_ += bytes;
        if (current_writes_ == 0) {
            ++accounting_errors_;
        } else {
            --current_writes_;
        }
    }

    void integrity_error() noexcept {
        active_ = true;
        ++integrity_errors_;
    }

    void transport_error() noexcept {
        active_ = true;
        ++transport_errors_;
    }

private:
    void emit() const noexcept {
        if (!active_) return;
        const uint64_t measured_cpu_end_ns = process_cpu_time_ns();
        const uint64_t connection_process_cpu_ns =
            measured_cpu_end_ns >= connection_cpu_start_ns_
                ? measured_cpu_end_ns - connection_cpu_start_ns_ : 0;
        const uint64_t measured_process_cpu_ns =
            measured_cpu_started_ &&
                    measured_cpu_end_ns >= measured_cpu_start_ns_
                ? measured_cpu_end_ns - measured_cpu_start_ns_ : 0;
        const bool valid = received_records_ == verified_records_ &&
            warmup_records_ + measured_records_ == received_records_ &&
            warmup_bytes_ + measured_bytes_ == received_bytes_ &&
            received_records_ == write_submissions_ &&
            write_submissions_ == write_completions_ &&
            received_bytes_ == verified_bytes_ &&
            received_bytes_ == submitted_bytes_ &&
            submitted_bytes_ == completed_bytes_ &&
            current_writes_ == 0 && max_writes_ <= 1 &&
            integrity_errors_ == 0 && transport_errors_ == 0 &&
            accounting_errors_ == 0;
        static std::mutex output_mutex;
        std::lock_guard lock(output_mutex);
        std::printf(
            "{\"schema_version\":\"%.*s\",\"kind\":\"server_connection\","
            "\"implementation\":\"%s\",\"received_records\":%llu,"
            "\"trial\":%llu,\"write_size_bytes\":%llu,"
            "\"warmup_records\":%llu,\"measured_records\":%llu,"
            "\"connection_process_cpu_ns\":%llu,"
            "\"measured_process_cpu_ns\":%llu,"
            "\"warmup_bytes\":%llu,\"measured_bytes\":%llu,"
            "\"verified_records\":%llu,\"write_submissions\":%llu,"
            "\"write_completions\":%llu,\"received_bytes\":%llu,"
            "\"verified_bytes\":%llu,\"submitted_bytes\":%llu,"
            "\"completed_bytes\":%llu,"
            "\"current_write_operations_in_flight\":%llu,"
            "\"max_write_operations_in_flight\":%llu,"
            "\"integrity_errors\":%llu,\"transport_errors\":%llu,"
            "\"accounting_errors\":%llu,\"valid\":%s}\n",
            static_cast<int>(kResultSchema.size()), kResultSchema.data(),
            implementation_,
            static_cast<unsigned long long>(received_records_),
            static_cast<unsigned long long>(trial_),
            static_cast<unsigned long long>(write_size_),
            static_cast<unsigned long long>(warmup_records_),
            static_cast<unsigned long long>(measured_records_),
            static_cast<unsigned long long>(connection_process_cpu_ns),
            static_cast<unsigned long long>(measured_process_cpu_ns),
            static_cast<unsigned long long>(warmup_bytes_),
            static_cast<unsigned long long>(measured_bytes_),
            static_cast<unsigned long long>(verified_records_),
            static_cast<unsigned long long>(write_submissions_),
            static_cast<unsigned long long>(write_completions_),
            static_cast<unsigned long long>(received_bytes_),
            static_cast<unsigned long long>(verified_bytes_),
            static_cast<unsigned long long>(submitted_bytes_),
            static_cast<unsigned long long>(completed_bytes_),
            static_cast<unsigned long long>(current_writes_),
            static_cast<unsigned long long>(max_writes_),
            static_cast<unsigned long long>(integrity_errors_),
            static_cast<unsigned long long>(transport_errors_),
            static_cast<unsigned long long>(accounting_errors_),
            valid ? "true" : "false");
        std::fflush(stdout);
    }

    const char* implementation_;
    uint64_t connection_cpu_start_ns_ = 0;
    bool active_ = false;
    uint64_t received_records_ = 0;
    uint64_t trial_ = 0;
    uint64_t write_size_ = 0;
    uint64_t warmup_records_ = 0;
    uint64_t measured_records_ = 0;
    bool measured_cpu_started_ = false;
    uint64_t measured_cpu_start_ns_ = 0;
    uint64_t warmup_bytes_ = 0;
    uint64_t measured_bytes_ = 0;
    uint64_t verified_records_ = 0;
    uint64_t write_submissions_ = 0;
    uint64_t write_completions_ = 0;
    uint64_t received_bytes_ = 0;
    uint64_t verified_bytes_ = 0;
    uint64_t submitted_bytes_ = 0;
    uint64_t completed_bytes_ = 0;
    uint64_t current_writes_ = 0;
    uint64_t max_writes_ = 0;
    uint64_t integrity_errors_ = 0;
    uint64_t transport_errors_ = 0;
    uint64_t accounting_errors_ = 0;
};

inline std::size_t server_max_record_size(const config& cfg) noexcept {
    return std::max({static_cast<std::size_t>(cfg.chunk_bytes),
                     static_cast<std::size_t>(cfg.message_size),
                     kMessageSizes.back()});
}

inline bool valid_record_size(uint32_t size, std::size_t maximum) noexcept {
    return size >= kRecordHeaderBytes && size <= maximum;
}

} // namespace bench
