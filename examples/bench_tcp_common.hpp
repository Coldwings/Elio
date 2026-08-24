/// @file bench_tcp_common.hpp
/// @brief Shared contract and evidence helpers for fair TCP loopback benchmarks.
#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <time.h>
#include <vector>

namespace bench {

inline constexpr std::string_view kResultSchema = "elio.tcp-loopback.v1";
inline constexpr std::string_view kReferencePeer = "posix-reference";
enum class workload { latency, message, bulk, all };
enum class phase : uint16_t { warmup = 1, measured = 2 };
inline constexpr std::size_t kRecordHeaderBytes = 32;
inline constexpr uint32_t kMaximumRecordBytes = uint32_t{64} << 20;

inline constexpr const char* workload_name(workload value) noexcept {
    switch (value) {
    case workload::latency: return "latency";
    case workload::message: return "message";
    case workload::bulk: return "bulk";
    case workload::all: return "all";
    }
    return "unknown";
}

struct config {
    workload type = workload::all;
    std::string host = "127.0.0.1";
    std::string peer_implementation;
    uint16_t port = 9876;
    uint64_t records = 100000;
    uint64_t warmup_records = 1000;
    // Zero derives a stable identifier from workload and record size.
    uint64_t trial = 0;
    uint32_t credit_window = 16;
    // Zero selects the standard size matrix. A non-zero value selects one
    // explicit record size so conformance runs emit one independently
    // attributable result per process.
    uint32_t message_size = 0;
    uint64_t bulk_bytes = uint64_t{1} << 30;
    uint32_t chunk_bytes = uint32_t{256} << 10;
    std::string json_path = "tcp-benchmark-results.jsonl";
};

class argument_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline void print_usage(const char* prog, const char* implementation) {
    std::printf(
        "TCP Loopback Benchmark: %s\nUsage: %s [options]\n\n"
        "  --host HOST\n  --port PORT\n"
        "  --peer-implementation NAME\n"
        "  --mode latency|message|bulk|all\n"
        "  --records COUNT\n  --warmup-records COUNT\n"
        "  --trial ID (0 derives an ID from workload and record size)\n"
        "  --credit-window COUNT\n  --bulk-bytes BYTES\n"
        "  --message-size BYTES (0 selects the standard size matrix)\n"
        "  --chunk-bytes BYTES\n  --json PATH\n  -h, --help\n",
        implementation, prog);
}

[[noreturn]] inline void fail_args(const char* prog, const char* implementation,
                                   const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    print_usage(prog, implementation);
    throw argument_error(message);
}

inline const char* require_value(int argc, char* argv[], int& index,
                                 const char* option,
                                 const char* implementation) {
    if (index + 1 >= argc) {
        fail_args(argv[0], implementation,
                  std::string("Missing value for ") + option);
    }
    return argv[++index];
}

inline uint64_t parse_unsigned_option(const char* prog,
                                      const char* implementation,
                                      std::string_view option,
                                      std::string_view value,
                                      uint64_t minimum,
                                      uint64_t maximum) {
    uint64_t parsed = 0;
    auto [end, ec] = std::from_chars(value.data(),
                                     value.data() + value.size(), parsed);
    if (ec != std::errc{} || end != value.data() + value.size() ||
        parsed < minimum || parsed > maximum) {
        fail_args(prog, implementation,
                  "Invalid value '" + std::string(value) + "' for " +
                      std::string(option));
    }
    return parsed;
}

inline bool parse_workload(std::string_view value, workload& out) noexcept {
    if (value == "latency") out = workload::latency;
    else if (value == "message") out = workload::message;
    else if (value == "bulk") out = workload::bulk;
    else if (value == "all") out = workload::all;
    else return false;
    return true;
}

inline config parse_args(int argc, char* argv[], const char* implementation) {
    config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        const auto value = [&](const char* name) {
            return require_value(argc, argv, i, name, implementation);
        };
        if (arg == "--host") cfg.host = value("--host");
        else if (arg == "--peer-implementation") {
            cfg.peer_implementation = value("--peer-implementation");
            if (cfg.peer_implementation.empty()) {
                fail_args(argv[0], implementation,
                          "--peer-implementation must not be empty");
            }
        }
        else if (arg == "--port") {
            cfg.port = static_cast<uint16_t>(parse_unsigned_option(
                argv[0], implementation, arg, value("--port"), 1, 65535));
        } else if (arg == "--mode") {
            const char* mode = value("--mode");
            if (!parse_workload(mode, cfg.type)) {
                fail_args(argv[0], implementation,
                          "Invalid mode '" + std::string(mode) + "'");
            }
        } else if (arg == "--records") {
            cfg.records = parse_unsigned_option(
                argv[0], implementation, arg, value("--records"), 1,
                std::numeric_limits<uint64_t>::max());
        } else if (arg == "--warmup-records") {
            cfg.warmup_records = parse_unsigned_option(
                argv[0], implementation, arg, value("--warmup-records"), 0,
                std::numeric_limits<uint64_t>::max());
        } else if (arg == "--trial") {
            cfg.trial = parse_unsigned_option(
                argv[0], implementation, arg, value("--trial"), 0,
                std::numeric_limits<uint64_t>::max());
        } else if (arg == "--credit-window") {
            cfg.credit_window = static_cast<uint32_t>(parse_unsigned_option(
                argv[0], implementation, arg, value("--credit-window"), 1,
                65536));
        } else if (arg == "--message-size") {
            cfg.message_size = static_cast<uint32_t>(parse_unsigned_option(
                argv[0], implementation, arg, value("--message-size"), 0,
                kMaximumRecordBytes));
            if (cfg.message_size != 0 &&
                cfg.message_size < kRecordHeaderBytes) {
                fail_args(argv[0], implementation,
                          "--message-size must be zero or at least 32 bytes");
            }
        } else if (arg == "--bulk-bytes") {
            cfg.bulk_bytes = parse_unsigned_option(
                argv[0], implementation, arg, value("--bulk-bytes"), 32,
                std::numeric_limits<uint64_t>::max());
        } else if (arg == "--chunk-bytes") {
            cfg.chunk_bytes = static_cast<uint32_t>(parse_unsigned_option(
                argv[0], implementation, arg, value("--chunk-bytes"), 32,
                kMaximumRecordBytes));
        } else if (arg == "--json") {
            cfg.json_path = value("--json");
            if (cfg.json_path.empty()) {
                fail_args(argv[0], implementation, "--json must not be empty");
            }
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0], implementation);
            std::exit(0);
        } else {
            fail_args(argv[0], implementation,
                      "Unknown option '" + std::string(arg) + "'");
        }
    }
    if (cfg.bulk_bytes % cfg.chunk_bytes != 0) {
        fail_args(argv[0], implementation,
                  "--bulk-bytes must be a multiple of --chunk-bytes");
    }
    return cfg;
}

inline constexpr std::array<std::size_t, 4> kMessageSizes{
    64, 1024, 4096, 65536};
inline std::vector<std::size_t> selected_message_sizes(const config& cfg) {
    if (cfg.message_size != 0) {
        return {cfg.message_size};
    }
    return {kMessageSizes.begin(), kMessageSizes.end()};
}
inline uint64_t trial_id(workload type, std::size_t size) noexcept {
    return (static_cast<uint64_t>(type) << 56) |
           static_cast<uint64_t>(size);
}
inline uint64_t selected_trial(const config& cfg, workload type,
                               std::size_t size) noexcept {
    return cfg.trial == 0 ? trial_id(type, size) : cfg.trial;
}
inline constexpr uint32_t kRecordMagic = 0x454c494fU;
inline constexpr uint16_t kRecordVersion = 1;

namespace detail {
inline void store16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v >> 8); p[1] = static_cast<uint8_t>(v);
}
inline void store32(uint8_t* p, uint32_t v) noexcept {
    for (int i = 0; i != 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - i * 8));
}
inline void store64(uint8_t* p, uint64_t v) noexcept {
    for (int i = 0; i != 8; ++i) p[i] = static_cast<uint8_t>(v >> (56 - i * 8));
}
inline uint16_t load16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((uint16_t{p[0]} << 8) | p[1]);
}
inline uint32_t load32(const uint8_t* p) noexcept {
    uint32_t v = 0; for (int i = 0; i != 4; ++i) v = (v << 8) | p[i]; return v;
}
inline uint64_t load64(const uint8_t* p) noexcept {
    uint64_t v = 0; for (int i = 0; i != 8; ++i) v = (v << 8) | p[i]; return v;
}
inline uint8_t payload_byte(std::size_t index) noexcept {
    const uint64_t x = index * 0x9e3779b97f4a7c15ULL;
    return static_cast<uint8_t>(x ^ (x >> 17) ^ (x >> 41));
}
inline uint32_t payload_tag(std::size_t payload_size) noexcept {
    return 0x5041594cU ^ static_cast<uint32_t>(payload_size) ^
           static_cast<uint32_t>(payload_size >> 32);
}
inline std::string json_escape(std::string_view text) {
    std::string out;
    for (unsigned char c : text) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += static_cast<char>(c);
    }
    return out;
}
} // namespace detail

struct record_header {
    uint32_t magic = 0;
    uint16_t version = 0;
    phase record_phase = phase::warmup;
    uint64_t trial = 0;
    uint64_t sequence = 0;
    uint32_t size = 0;
    uint32_t payload_checksum = 0;
};

inline record_header decode_record_header(std::span<const uint8_t> record) noexcept {
    if (record.size() < kRecordHeaderBytes) return {};
    return {detail::load32(record.data()), detail::load16(record.data() + 4),
            static_cast<phase>(detail::load16(record.data() + 6)),
            detail::load64(record.data() + 8),
            detail::load64(record.data() + 16),
            detail::load32(record.data() + 24),
            detail::load32(record.data() + 28)};
}

inline bool initialize_record(std::span<uint8_t> record) noexcept {
    if (record.size() < kRecordHeaderBytes ||
        record.size() > std::numeric_limits<uint32_t>::max()) return false;
    auto payload = record.subspan(kRecordHeaderBytes);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = detail::payload_byte(i);
    }
    detail::store32(record.data() + 28, detail::payload_tag(payload.size()));
    return true;
}

inline bool stamp_record(std::span<uint8_t> record, phase record_phase,
                         uint64_t trial, uint64_t sequence) noexcept {
    if (record.size() < kRecordHeaderBytes ||
        record.size() > std::numeric_limits<uint32_t>::max()) return false;
    detail::store32(record.data(), kRecordMagic);
    detail::store16(record.data() + 4, kRecordVersion);
    detail::store16(record.data() + 6, static_cast<uint16_t>(record_phase));
    detail::store64(record.data() + 8, trial);
    detail::store64(record.data() + 16, sequence);
    detail::store32(record.data() + 24, static_cast<uint32_t>(record.size()));
    return true;
}

inline bool build_record(std::span<uint8_t> record, phase record_phase,
                         uint64_t trial, uint64_t sequence) noexcept {
    return initialize_record(record) &&
           stamp_record(record, record_phase, trial, sequence);
}

inline std::vector<uint8_t> build_record(std::size_t size, phase p,
                                         uint64_t trial, uint64_t sequence) {
    std::vector<uint8_t> record(size);
    if (!build_record(record, p, trial, sequence)) {
        throw std::invalid_argument("invalid TCP benchmark record size");
    }
    return record;
}

enum class record_error { none, too_short, magic, version, phase, trial,
                          sequence, size, checksum, payload };

inline record_error validate_record(std::span<const uint8_t> record,
                                    phase expected_phase,
                                    uint64_t expected_trial,
                                    uint64_t expected_sequence,
                                    std::size_t expected_size) noexcept {
    if (record.size() < kRecordHeaderBytes) return record_error::too_short;
    const auto h = decode_record_header(record);
    if (h.magic != kRecordMagic) return record_error::magic;
    if (h.version != kRecordVersion) return record_error::version;
    if (h.record_phase != expected_phase) return record_error::phase;
    if (h.trial != expected_trial) return record_error::trial;
    if (h.sequence != expected_sequence) return record_error::sequence;
    if (h.size != record.size() || record.size() != expected_size) return record_error::size;
    const auto payload = record.subspan(kRecordHeaderBytes);
    if (detail::payload_tag(payload.size()) != h.payload_checksum) {
        return record_error::checksum;
    }
    for (std::size_t i = 0; i < payload.size(); ++i) {
        if (payload[i] != detail::payload_byte(i)) {
            return record_error::payload;
        }
    }
    return record_error::none;
}

struct counters {
    uint64_t write_submissions = 0, write_completions = 0;
    uint64_t sent_records = 0, received_records = 0, verified_records = 0;
    uint64_t sent_bytes = 0, received_bytes = 0, verified_bytes = 0;
    uint64_t current_writes_in_flight = 0, max_writes_in_flight = 0;
    uint64_t max_unacknowledged_records = 0;
    uint64_t write_size_errors = 0, records_per_write_errors = 0;
    uint64_t write_completion_errors = 0, sequence_errors = 0;
    uint64_t payload_errors = 0, transport_errors = 0, phase_errors = 0;

    void begin_write(std::size_t bytes, uint64_t logical_records,
                     std::size_t expected_size) noexcept {
        ++write_submissions; ++current_writes_in_flight;
        max_writes_in_flight = std::max(max_writes_in_flight,
                                        current_writes_in_flight);
        if (bytes != expected_size) ++write_size_errors;
        if (logical_records != 1) ++records_per_write_errors;
    }
    void complete_write(std::size_t bytes, uint64_t logical_records,
                        std::size_t expected_size) noexcept {
        ++write_completions;
        if (current_writes_in_flight == 0) ++write_completion_errors;
        else --current_writes_in_flight;
        if (bytes != expected_size) ++write_size_errors;
        if (logical_records != 1) ++records_per_write_errors;
        sent_records += logical_records; sent_bytes += bytes;
    }
    void receive_record(std::size_t bytes) noexcept {
        ++received_records; received_bytes += bytes;
    }
    void verify_record(std::size_t bytes) noexcept {
        ++verified_records; verified_bytes += bytes;
    }
    void observe_unacknowledged(uint64_t value) noexcept {
        max_unacknowledged_records = std::max(max_unacknowledged_records, value);
    }
    uint64_t error_count() const noexcept {
        return write_size_errors + records_per_write_errors +
               write_completion_errors + sequence_errors + payload_errors +
               transport_errors + phase_errors;
    }
};

struct latency_summary {
    uint64_t p50_ns = 0, p95_ns = 0, p99_ns = 0, p999_ns = 0, max_ns = 0;
    static latency_summary compute(std::vector<uint64_t> samples) {
        latency_summary out;
        if (samples.empty()) return out;
        std::sort(samples.begin(), samples.end());
        const auto q = [&](double fraction) {
            auto rank = static_cast<std::size_t>(std::ceil(fraction * samples.size()));
            return samples[std::min(samples.size() - 1, rank == 0 ? 0 : rank - 1)];
        };
        out.p50_ns = q(.5); out.p95_ns = q(.95); out.p99_ns = q(.99);
        out.p999_ns = q(.999); out.max_ns = samples.back(); return out;
    }
};

inline uint64_t process_cpu_time_ns() noexcept {
    struct timespec value {};
    if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(value.tv_nsec);
}

struct result {
    std::string implementation;
    std::string role = "client";
    std::string peer = std::string(kReferencePeer);
    std::string counter_scope = "adapter";
    workload workload_type = workload::message;
    uint64_t trial = 0;
    std::size_t write_size = 0;
    uint32_t credit_window = 1;
    uint64_t warmup_expected_records = 0, measured_expected_records = 0;
    uint64_t elapsed_ns = 0;
    uint64_t process_cpu_elapsed_ns = 0;
    bool warmup_drained = false;
    counters warmup, measured;
    std::vector<uint64_t> latency_samples_ns;

    std::vector<std::string> invariant_failures() const {
        std::vector<std::string> failures;
        const auto check = [&](const counters& c, uint64_t count,
                               std::string_view prefix) {
            const uint64_t bytes = count * static_cast<uint64_t>(write_size);
            const auto add = [&](std::string_view field) {
                failures.emplace_back(std::string(prefix) + "." + std::string(field));
            };
            if (c.sent_records != count) add("sent_records");
            if (c.write_submissions != count) add("write_submissions");
            if (c.write_completions != count) add("write_completions");
            if (c.received_records != count) add("received_records");
            if (c.verified_records != count) add("verified_records");
            if (c.sent_bytes != bytes) add("sent_bytes");
            if (c.received_bytes != bytes) add("received_bytes");
            if (c.verified_bytes != bytes) add("verified_bytes");
            if (c.max_writes_in_flight > 1) add("max_writes_in_flight");
            if (c.current_writes_in_flight != 0) add("current_writes_in_flight");
            if (c.max_unacknowledged_records > credit_window) add("credit_window");
            if (c.error_count() != 0) add("errors");
        };
        check(warmup, warmup_expected_records, "warmup");
        check(measured, measured_expected_records, "measured");
        if (!warmup_drained) failures.emplace_back("warmup.not_drained");
        if (elapsed_ns == 0) failures.emplace_back("timing.elapsed_ns");
        if (workload_type == workload::latency &&
            latency_samples_ns.size() != measured_expected_records) {
            failures.emplace_back("latency.sample_count");
        }
        return failures;
    }
    bool valid() const { return invariant_failures().empty(); }
    double records_per_second() const noexcept {
        return elapsed_ns == 0 ? 0.0 : measured.verified_records * 1e9 / elapsed_ns;
    }
    double mib_per_second() const noexcept {
        return elapsed_ns == 0 ? 0.0 :
            (measured.verified_bytes / (1024.0 * 1024.0)) * 1e9 / elapsed_ns;
    }
};

inline void append_counters_json(std::string& out, const counters& c) {
    out += "{\"write_submissions\":" + std::to_string(c.write_submissions);
    out += ",\"write_completions\":" + std::to_string(c.write_completions);
    out += ",\"sent_records\":" + std::to_string(c.sent_records);
    out += ",\"received_records\":" + std::to_string(c.received_records);
    out += ",\"verified_records\":" + std::to_string(c.verified_records);
    out += ",\"sent_bytes\":" + std::to_string(c.sent_bytes);
    out += ",\"received_bytes\":" + std::to_string(c.received_bytes);
    out += ",\"verified_bytes\":" + std::to_string(c.verified_bytes);
    out += ",\"max_write_operations_in_flight\":" +
           std::to_string(c.max_writes_in_flight);
    out += ",\"current_write_operations_in_flight\":" +
           std::to_string(c.current_writes_in_flight);
    out += ",\"max_unacknowledged_records\":" +
           std::to_string(c.max_unacknowledged_records);
    out += ",\"write_size_errors\":" + std::to_string(c.write_size_errors);
    out += ",\"records_per_write_errors\":" +
           std::to_string(c.records_per_write_errors);
    out += ",\"write_completion_errors\":" +
           std::to_string(c.write_completion_errors);
    out += ",\"sequence_errors\":" + std::to_string(c.sequence_errors);
    out += ",\"payload_errors\":" + std::to_string(c.payload_errors);
    out += ",\"transport_errors\":" + std::to_string(c.transport_errors);
    out += ",\"phase_errors\":" + std::to_string(c.phase_errors);
    out += ",\"error_count\":" + std::to_string(c.error_count()) + "}";
}

inline std::string to_json_line(const result& value) {
    const auto failures = value.invariant_failures();
    const auto latency = latency_summary::compute(value.latency_samples_ns);
    std::string out = "{\"schema_version\":\"" + std::string(kResultSchema) +
        "\",\"implementation\":\"" + detail::json_escape(value.implementation) +
        "\",\"role\":\"" + detail::json_escape(value.role) +
        "\",\"peer\":\"" + detail::json_escape(value.peer) +
        "\",\"counter_scope\":\"" +
        detail::json_escape(value.counter_scope) +
        "\",\"workload\":\"" + workload_name(value.workload_type) + "\"";
    out += ",\"trial\":" + std::to_string(value.trial);
    out += ",\"configuration\":{\"write_size_bytes\":" +
           std::to_string(value.write_size) + ",\"credit_window\":" +
           std::to_string(value.credit_window) + ",\"warmup_records\":" +
           std::to_string(value.warmup_expected_records) +
           ",\"measured_records\":" +
           std::to_string(value.measured_expected_records) + "}";
    out += ",\"elapsed_ns\":" + std::to_string(value.elapsed_ns) +
           ",\"process_cpu_elapsed_ns\":" +
           std::to_string(value.process_cpu_elapsed_ns) +
           ",\"latency_sample_count\":" +
           std::to_string(value.latency_samples_ns.size()) +
           ",\"warmup_drained\":" + (value.warmup_drained ? "true" : "false") +
           ",\"counters\":{\"warmup\":";
    append_counters_json(out, value.warmup); out += ",\"measured\":";
    append_counters_json(out, value.measured); out += "},\"valid\":";
    out += failures.empty() ? "true" : "false";
    out += ",\"invariant_failures\":[";
    for (std::size_t i = 0; i < failures.size(); ++i) {
        if (i) out += ',';
        out += '"' + detail::json_escape(failures[i]) + '"';
    }
    out += "]";
    if (failures.empty()) {
        out += ",\"metrics\":{";
        if (value.workload_type != workload::bulk) {
            out += "\"records_per_second\":" +
                   std::to_string(value.records_per_second()) + ',';
        }
        out += "\"mib_per_second\":" + std::to_string(value.mib_per_second());
        if (value.workload_type == workload::latency) {
            out += ",\"latency_ns\":{\"p50\":" + std::to_string(latency.p50_ns) +
                   ",\"p95\":" + std::to_string(latency.p95_ns) +
                   ",\"p99\":" + std::to_string(latency.p99_ns) +
                   ",\"p99_9\":" + std::to_string(latency.p999_ns) +
                   ",\"max\":" + std::to_string(latency.max_ns) + "}";
        }
        out += "}";
    } else out += ",\"metrics\":null";
    out += "}"; return out;
}

inline bool append_jsonl(std::string_view path, const result& value,
                         std::string* error = nullptr) {
    std::ofstream file(std::string(path), std::ios::app);
    if (!file) { if (error) *error = "failed to open JSONL output"; return false; }
    file << to_json_line(value) << '\n';
    if (!file) { if (error) *error = "failed to write JSONL output"; return false; }
    return true;
}

inline void print_human(const result& value) {
    const auto failures = value.invariant_failures();
    if (!failures.empty()) {
        std::printf("%s %zuB INVALID (%zu invariant failures)\n",
                    workload_name(value.workload_type), value.write_size,
                    failures.size()); return;
    }
    if (value.workload_type == workload::latency) {
        const auto s = latency_summary::compute(value.latency_samples_ns);
        std::printf("latency %zuB records/s=%.0f MiB/s=%.2f p50=%lluns "
                    "p95=%lluns p99=%lluns p99.9=%lluns max=%lluns\n",
                    value.write_size, value.records_per_second(),
                    value.mib_per_second(), (unsigned long long)s.p50_ns,
                    (unsigned long long)s.p95_ns, (unsigned long long)s.p99_ns,
                    (unsigned long long)s.p999_ns, (unsigned long long)s.max_ns);
    } else if (value.workload_type == workload::message) {
        std::printf("message %zuB records/s=%.0f MiB/s=%.2f\n", value.write_size,
                    value.records_per_second(), value.mib_per_second());
    } else {
        std::printf("bulk %zuB MiB/s=%.2f\n", value.write_size,
                    value.mib_per_second());
    }
}

} // namespace bench
