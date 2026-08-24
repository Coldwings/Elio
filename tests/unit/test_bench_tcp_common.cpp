#include <catch2/catch_test_macros.hpp>

#include "../../examples/bench_tcp_common.hpp"
#include "../../examples/bench_tcp_server_protocol.hpp"

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

bench::config parse(std::initializer_list<const char*> arguments) {
    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1);
    storage.emplace_back("bench");
    for (const char* argument : arguments) storage.emplace_back(argument);
    std::vector<char*> argv;
    for (auto& item : storage) argv.push_back(item.data());
    return bench::parse_args(static_cast<int>(argv.size()), argv.data(), "Test");
}

void record_success(bench::counters& counters, std::size_t bytes,
                    uint64_t unacknowledged = 1) {
    counters.begin_write(bytes, 1, bytes);
    counters.observe_unacknowledged(unacknowledged);
    counters.complete_write(bytes, 1, bytes);
    counters.receive_record(bytes);
    counters.verify_record(bytes);
}

bench::result valid_result(bench::workload type = bench::workload::message) {
    bench::result result;
    result.implementation = "Test";
    result.role = "server";
    result.peer = "reference-client";
    result.counter_scope = "driver";
    result.workload_type = type;
    result.trial = 17;
    result.write_size = 64;
    result.credit_window = 16;
    result.warmup_expected_records = 1;
    result.measured_expected_records = 2;
    result.elapsed_ns = 1000000;
    result.process_cpu_elapsed_ns = 400000;
    result.warmup_drained = true;
    record_success(result.warmup, 64);
    record_success(result.measured, 64, 1);
    record_success(result.measured, 64, 2);
    if (type == bench::workload::latency) {
        result.latency_samples_ns = {200, 100};
    }
    return result;
}

bool has_failure(const bench::result& result, std::string_view suffix) {
    const auto failures = result.invariant_failures();
    return std::any_of(failures.begin(), failures.end(), [&](const auto& item) {
        return item.find(suffix) != std::string::npos;
    });
}

} // namespace

TEST_CASE("TCP benchmark fixed-work defaults are explicit", "[bench][tcp][args]") {
    const auto cfg = parse({});
    CHECK(cfg.type == bench::workload::all);
    CHECK(cfg.records == 100000);
    CHECK(cfg.warmup_records == 1000);
    CHECK(cfg.trial == 0);
    CHECK(cfg.credit_window == 16);
    CHECK(cfg.bulk_bytes == (uint64_t{1} << 30));
    CHECK(cfg.chunk_bytes == (uint32_t{256} << 10));
}

TEST_CASE("TCP benchmark parser accepts the fair workload controls",
          "[bench][tcp][args]") {
    const auto cfg = parse({"--host", "localhost", "--port", "3210",
                            "--mode", "message", "--records", "42",
                            "--warmup-records", "3", "--credit-window", "8",
                            "--trial", "18446744073709551615",
                            "--bulk-bytes", "4096", "--chunk-bytes", "64",
                            "--json", "results.jsonl"});
    CHECK(cfg.host == "localhost");
    CHECK(cfg.port == 3210);
    CHECK(cfg.type == bench::workload::message);
    CHECK(cfg.records == 42);
    CHECK(cfg.warmup_records == 3);
    CHECK(cfg.credit_window == 8);
    CHECK(cfg.trial == std::numeric_limits<uint64_t>::max());
    CHECK(cfg.bulk_bytes == 4096);
    CHECK(cfg.chunk_bytes == 64);
    CHECK(cfg.json_path == "results.jsonl");
}

TEST_CASE("TCP benchmark trial selection is stable and overridable",
          "[bench][tcp][args][protocol]") {
    const bench::config automatic;
    CHECK(bench::selected_trial(
              automatic, bench::workload::latency, 64) ==
          bench::trial_id(bench::workload::latency, 64));
    CHECK(bench::trial_id(bench::workload::latency, 64) !=
          bench::trial_id(bench::workload::message, 64));

    auto explicit_trial = automatic;
    explicit_trial.trial = 987654321;
    CHECK(bench::selected_trial(
              explicit_trial, bench::workload::bulk, 4096) == 987654321);
}

TEST_CASE("TCP process CPU clock is monotonic", "[bench][tcp][cpu]") {
    const auto before = bench::process_cpu_time_ns();
    volatile uint64_t value = 0;
    for (uint64_t i = 0; i != 1000; ++i) value = value + i;
    const auto after = bench::process_cpu_time_ns();
    CHECK(after >= before);
}

TEST_CASE("TCP server evidence exposes conservative and measured CPU fields",
          "[bench][tcp][cpu][json]") {
    std::fflush(stdout);
    FILE* capture = std::tmpfile();
    REQUIRE(capture != nullptr);
    const int saved_stdout = ::dup(STDOUT_FILENO);
    REQUIRE(saved_stdout >= 0);
    REQUIRE(::dup2(::fileno(capture), STDOUT_FILENO) >= 0);

    {
        bench::server_connection_evidence evidence("test-server");
        for (const auto record_phase : {bench::phase::warmup,
                                        bench::phase::measured}) {
            const auto record = bench::build_record(
                64, record_phase, 4242, 0);
            evidence.receive_record(record);
            evidence.verify_record(record.size());
            evidence.begin_write(record.size());
            evidence.complete_write(record.size());
        }
    }
    std::fflush(stdout);
    REQUIRE(::dup2(saved_stdout, STDOUT_FILENO) >= 0);
    ::close(saved_stdout);

    std::rewind(capture);
    std::string json;
    std::array<char, 1024> buffer{};
    while (const auto bytes =
               std::fread(buffer.data(), 1, buffer.size(), capture)) {
        json.append(buffer.data(), bytes);
    }
    std::fclose(capture);
    CHECK(json.find("\"trial\":4242") != std::string::npos);
    CHECK(json.find("\"connection_process_cpu_ns\":") !=
          std::string::npos);
    CHECK(json.find("\"measured_process_cpu_ns\":") !=
          std::string::npos);
    CHECK(json.find("\"valid\":true") != std::string::npos);
}

TEST_CASE("TCP benchmark parser rejects ambiguous old and invalid fixed work",
          "[bench][tcp][args]") {
    CHECK_THROWS_AS(parse({"--mode", "streaming"}), bench::argument_error);
    CHECK_THROWS_AS(parse({"--records", "0"}), bench::argument_error);
    CHECK_THROWS_AS(parse({"--credit-window", "0"}), bench::argument_error);
    CHECK_THROWS_AS(parse({"--bulk-bytes", "65", "--chunk-bytes", "64"}),
                    bench::argument_error);
}

TEST_CASE("TCP records use a stable 32-byte header and deterministic payload",
          "[bench][tcp][protocol]") {
    REQUIRE(bench::kRecordHeaderBytes == 32);
    auto record = bench::build_record(64, bench::phase::measured, 77, 9);
    const auto header = bench::decode_record_header(record);
    CHECK(header.magic == bench::kRecordMagic);
    CHECK(header.version == bench::kRecordVersion);
    CHECK(header.record_phase == bench::phase::measured);
    CHECK(header.trial == 77);
    CHECK(header.sequence == 9);
    CHECK(header.size == 64);
    CHECK(bench::validate_record(record, bench::phase::measured, 77, 9, 64) ==
          bench::record_error::none);
    CHECK(bench::build_record(64, bench::phase::measured, 77, 9) == record);
}

TEST_CASE("TCP record validation rejects corruption and sequence mismatch",
          "[bench][tcp][protocol][negative]") {
    auto record = bench::build_record(4096, bench::phase::warmup, 4, 11);
    CHECK(bench::validate_record(record, bench::phase::warmup, 4, 12, 4096) ==
          bench::record_error::sequence);
    record.back() ^= 0x40;
    CHECK(bench::validate_record(record, bench::phase::warmup, 4, 11, 4096) ==
          bench::record_error::payload);
    record = bench::build_record(4096, bench::phase::warmup, 4, 11);
    record[31] ^= 0x01;
    CHECK(bench::validate_record(record, bench::phase::warmup, 4, 11, 4096) ==
          bench::record_error::checksum);
}

TEST_CASE("TCP server protocol enforces phase and sequence transitions",
          "[bench][tcp][protocol]") {
    bench::server_protocol protocol;
    CHECK(protocol.accept(
        bench::build_record(64, bench::phase::warmup, 9, 0)));
    CHECK(protocol.accept(
        bench::build_record(64, bench::phase::warmup, 9, 1)));
    CHECK(protocol.accept(
        bench::build_record(64, bench::phase::measured, 9, 0)));
    CHECK_FALSE(protocol.accept(
        bench::build_record(64, bench::phase::measured, 9, 2)));
}

TEST_CASE("TCP server protocol supports zero warmup and rejects corruption",
          "[bench][tcp][protocol][negative]") {
    bench::server_protocol protocol;
    auto measured = bench::build_record(4096, bench::phase::measured, 17, 0);
    CHECK(protocol.accept(measured));
    measured = bench::build_record(4096, bench::phase::measured, 17, 1);
    measured.back() ^= 0x80;
    CHECK_FALSE(protocol.accept(measured));

    bench::server_protocol new_trial;
    CHECK_FALSE(new_trial.accept(
        bench::build_record(64, bench::phase::warmup, 20, 1)));
}

TEST_CASE("TCP result accepts equivalent fixed-work counters",
          "[bench][tcp][invariants]") {
    const auto result = valid_result();
    CHECK(result.valid());
    CHECK(result.measured.sent_records == result.measured.write_submissions);
    CHECK(result.measured.write_submissions == result.measured.received_records);
    CHECK(result.measured.received_records == result.measured.verified_records);
}

TEST_CASE("TCP result rejects the former adapter-only record batching",
          "[bench][tcp][invariants][negative]") {
    auto result = valid_result();
    result.measured = {};
    result.measured.begin_write(64 * 16, 16, 64);
    result.measured.complete_write(64 * 16, 16, 64);
    for (int i = 0; i < 16; ++i) {
        result.measured.receive_record(64);
        result.measured.verify_record(64);
    }
    result.measured_expected_records = 16;
    CHECK_FALSE(result.valid());
    CHECK(result.measured.write_size_errors > 0);
    CHECK(result.measured.records_per_write_errors > 0);
    CHECK(has_failure(result, "write_submissions"));
}

TEST_CASE("TCP result rejects overlapping same-stream write operations",
          "[bench][tcp][invariants][negative]") {
    auto result = valid_result();
    result.measured.begin_write(64, 1, 64);
    result.measured.begin_write(64, 1, 64);
    CHECK(result.measured.max_writes_in_flight == 2);
    CHECK_FALSE(result.valid());
    CHECK(has_failure(result, "max_writes_in_flight"));
}

TEST_CASE("TCP JSONL names the contract, peer, counters, and explicit metrics",
          "[bench][tcp][json]") {
    const auto json = bench::to_json_line(valid_result(bench::workload::latency));
    CHECK(json.find("\"schema_version\":\"elio.tcp-loopback.v1\"") !=
          std::string::npos);
    CHECK(json.find("\"role\":\"server\"") != std::string::npos);
    CHECK(json.find("\"peer\":\"reference-client\"") != std::string::npos);
    CHECK(json.find("\"counter_scope\":\"driver\"") != std::string::npos);
    CHECK(json.find("\"max_write_operations_in_flight\":1") !=
          std::string::npos);
    CHECK(json.find("\"current_write_operations_in_flight\":0") !=
          std::string::npos);
    CHECK(json.find("\"write_completion_errors\":0") != std::string::npos);
    CHECK(json.find("\"latency_sample_count\":2") != std::string::npos);
    CHECK(json.find("\"trial\":17") != std::string::npos);
    CHECK(json.find("\"process_cpu_elapsed_ns\":400000") !=
          std::string::npos);
    CHECK(json.find("\"mib_per_second\"") != std::string::npos);
    CHECK(json.find("\"p50\"") != std::string::npos);
    CHECK(json.find("\"p95\"") != std::string::npos);
    CHECK(json.find("\"p99_9\"") != std::string::npos);
    CHECK(json.find("IOPS") == std::string::npos);
}

TEST_CASE("TCP bulk JSON reports bytes rather than a synthetic record rate",
          "[bench][tcp][json]") {
    const auto json = bench::to_json_line(valid_result(bench::workload::bulk));
    CHECK(json.find("\"mib_per_second\"") != std::string::npos);
    CHECK(json.find("\"records_per_second\"") == std::string::npos);
}
