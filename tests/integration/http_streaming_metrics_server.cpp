// Fixed-work real-transport diagnostic, not an isolated server-capacity benchmark.
#include <elio/http/http_parser.hpp>
#include <elio/http/http_response_sender.hpp>
#include <elio/net/tcp.hpp>
#include <elio/tls/tls_stream.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/log/logger.hpp>

#include <openssl/crypto.h>
#include <time.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#ifndef ELIO_HTTP_METRICS_BUILD_TYPE
#define ELIO_HTTP_METRICS_BUILD_TYPE "unspecified"
#endif
#ifndef ELIO_HTTP_METRICS_BUILD_HEAD
#define ELIO_HTTP_METRICS_BUILD_HEAD "unknown"
#endif
#ifndef ELIO_HTTP_METRICS_BUILD_DIRTY
#define ELIO_HTTP_METRICS_BUILD_DIRTY true
#endif

namespace {
using namespace elio;
using namespace elio::http;
using namespace std::chrono_literals;
constexpr size_t body_bytes = 4 * 1024 * 1024;
constexpr size_t block_bytes = 64 * 1024;
constexpr size_t measured_responses = 16;

struct options {
    std::string transport;
    std::string mode;
    std::string trial;
    std::string certificate;
    std::string key;
};

options parse_options(int argc, char** argv) {
    options out;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) throw std::invalid_argument("missing option value");
        const std::string_view flag(argv[i]);
        std::string* target = nullptr;
        if (flag == "--transport") target = &out.transport;
        else if (flag == "--mode") target = &out.mode;
        else if (flag == "--trial") target = &out.trial;
        else if (flag == "--cert") target = &out.certificate;
        else if (flag == "--key") target = &out.key;
        else throw std::invalid_argument("unknown option");
        if (!target->empty()) throw std::invalid_argument("duplicate option");
        *target = argv[i + 1];
    }
    if (out.transport != "tcp" && out.transport != "tls") throw std::invalid_argument("invalid transport");
    if (out.mode != "complete" && out.mode != "known_stream" && out.mode != "chunked") {
        throw std::invalid_argument("invalid framing mode");
    }
    if (out.trial.empty() || out.trial.size() > 64 ||
        out.trial.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) {
        throw std::invalid_argument("invalid trial identifier");
    }
    if (out.transport == "tls" && (out.certificate.empty() || out.key.empty())) {
        throw std::invalid_argument("TLS requires --cert and --key");
    }
    return out;
}

uint64_t process_cpu_ns() {
    timespec value{};
    if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0) {
        throw std::runtime_error("process CPU clock failed");
    }
    return static_cast<uint64_t>(value.tv_sec) * 1000000000ULL + static_cast<uint64_t>(value.tv_nsec);
}

int64_t monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct response_evidence {
    const char* phase = "warmup";
    size_t sequence = 0;
    response_send_result sent;
    uint64_t cpu_ns = 0;
};

struct evidence {
    std::array<response_evidence, measured_responses + 2> responses{};
    size_t response_count = 0;
    size_t measured_count = 0;
    uint64_t confirmed = 0;
    uint64_t cpu_ns = 0;
    bool probe_success = false;
    bool success = false;
    std::string tls_version = "none";
    std::string tls_cipher = "none";
    std::string error;
    const char* error_type = "none";
    const char* stage = "accept";
    std::string request_phase;
    size_t request_sequence = 0;
    size_t request_bytes = 0;
    bool request_complete = false;
    bool read_result_available = false;
    io::io_result last_read{};
    int64_t request_started_ns = 0;
    int64_t request_elapsed_ns = 0;
};

// Only the main thread supervises deadlines; it does not touch the stream.
// All failure paths await cooperative cancellation before stream destruction.
struct supervision {
    coro::cancel_source stop;
    std::atomic<int64_t> phase_deadline{0};
    void start_bounded_phase() { phase_deadline.store(monotonic_ns() + 10000000000LL); }
    void sending() { phase_deadline.store(0); }
};

template<typename Stream>
coro::task<void> read_request(Stream& stream, const options& config,
                              std::string_view phase, size_t sequence,
                              supervision& control, evidence& result) {
    control.start_bounded_phase();
    result.stage = "request_read";
    result.request_phase = phase;
    result.request_sequence = sequence;
    result.request_bytes = 0;
    result.request_complete = false;
    result.read_result_available = false;
    result.request_started_ns = monotonic_ns();
    request_parser parser;
    parser.set_max_headers(16);
    parser.set_max_header_size(1024);
    std::array<char, 2048> buffer{};
    size_t received = 0;
    while (!parser.is_complete()) {
        result.read_result_available = false;
        const auto input = co_await stream.read(buffer.data(), buffer.size(), control.stop.get_token());
        result.last_read = input;
        result.read_result_available = true;
        result.request_elapsed_ns = monotonic_ns() - result.request_started_ns;
        if (input.result <= 0) throw std::runtime_error("request read failed");
        received += static_cast<size_t>(input.result);
        result.request_bytes = received;
        if (received > 8192) throw std::runtime_error("request exceeds fixture limit");
        const auto parsed = parser.parse({buffer.data(), static_cast<size_t>(input.result)});
        if (parsed.first == parse_result::error) throw std::runtime_error("invalid request");
    }
    const auto& headers = parser.get_headers();
    if (parser.get_method() != method::GET || parser.path() != "/" ||
        !parser.query().empty() || parser.version() != "HTTP/1.1" ||
        !parser.body().empty() || parser.buffered_input_size() != 0 ||
        headers.get("X-Trial-ID") != config.trial ||
        headers.get("X-Phase") != phase || headers.get("X-Sequence") != std::to_string(sequence) ||
        headers.get_all("X-Trial-ID").size() != 1 ||
        headers.get_all("X-Phase").size() != 1 || headers.get_all("X-Sequence").size() != 1 ||
        !headers.keep_alive("HTTP/1.1")) {
        throw std::runtime_error("request does not match fixed trial sequence");
    }
    result.request_complete = true;
}

template<typename Stream>
coro::task<void> send_trial(Stream& stream, const options& config,
                            const std::string& payload, reply& complete,
                            supervision& control, evidence& result) {
    for (size_t index = 0; index < measured_responses + 2; ++index) {
        const bool measured = index > 0 && index <= measured_responses;
        const bool probe = index == measured_responses + 1;
        const char* phase = probe ? "probe" : measured ? "measure" : "warmup";
        const size_t sequence = measured ? index - 1 : 0;
        co_await read_request(stream, config, phase, sequence, control, result);
        result.stage = "response_send";

        // Source storage and selected-reply construction are outside the
        // measured send interval. The shared complete body is not recopied.
        reply selected;
        reply* outgoing = &selected;
        if (probe) {
            selected = response::ok("ok");
        } else if (config.mode == "complete") {
            outgoing = &complete;
        } else {
            selected = streaming_response(status::ok,
                [&payload](body_writer& writer, coro::cancel_token token) -> coro::task<send_result> {
                    for (size_t offset = 0; offset < payload.size(); offset += block_bytes) {
                        auto sent = co_await writer.write(std::string_view(payload).substr(offset, block_bytes), token);
                        if (!sent.success()) co_return sent;
                    }
                    co_return send_result{};
                }, config.mode == "known_stream" ? std::optional<uint64_t>(body_bytes) : std::nullopt);
        }
        std::visit([&](auto& head) {
            head.set_header("X-Trial-ID", config.trial);
            head.set_header("X-Phase", phase);
            head.set_header("X-Sequence", std::to_string(sequence));
        }, *outgoing);
        control.sending();
        const auto before = measured ? process_cpu_ns() : 0;
        auto sent = co_await send_response(stream, *outgoing, method::GET, "HTTP/1.1", true,
                                           control.stop.get_token(), 5s);
        const auto after = measured ? process_cpu_ns() : 0;
        if (after < before) throw std::runtime_error("CPU clock regressed");
        auto& row = result.responses[result.response_count++];
        row = {phase, sequence, sent, after - before};
        if (!sent.success() || !sent.reusable ||
            sent.result.confirmed_body_bytes != (probe ? 2 : body_bytes)) {
            throw std::runtime_error("response failed completion or byte accounting");
        }
        if (measured) {
            ++result.measured_count;
            result.confirmed += sent.result.confirmed_body_bytes;
            result.cpu_ns += row.cpu_ns;
        }
        if (probe) result.probe_success = true;
    }
    result.success = true;
}

coro::task<void> run_connection(net::tcp_listener& listener, const options& config,
                                const std::string& payload, reply& complete,
                                tls::tls_context* tls_context, supervision& control,
                                evidence& result) {
    auto accepted = co_await listener.accept(control.stop.get_token());
    if (!accepted) throw std::runtime_error("accept failed");
    if (!tls_context) {
        co_await send_trial(*accepted, config, payload, complete, control, result);
    } else {
        tls::tls_stream stream(std::move(*accepted), *tls_context);
        struct abandon_tls {
            tls::tls_stream& stream;
            ~abandon_tls() { stream.shutdown_socket(); }
        } cleanup{stream};
        // Accept and handshake share the original startup budget.
        result.stage = "tls_handshake";
        if (!co_await stream.handshake(control.stop.get_token())) throw std::runtime_error("TLS handshake failed");
        result.tls_version = stream.version();
        result.tls_cipher = stream.cipher();
        co_await send_trial(stream, config, payload, complete, control, result);
        // Transport completion is not proof of peer consumption. The probe
        // request proves measured responses were decoded before this teardown.
    }
}

// Metadata strings can contain quotes (compiler/build configuration), so JSON
// escaping is required even though wire trial IDs have a restricted alphabet.
std::string json_quote(std::string_view value) {
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 0x20) {
            result += "\\u00";
            result += hex[c >> 4]; result += hex[c & 15];
        } else result += static_cast<char>(c);
    }
    return result + '"';
}

void print_result(const options& config, const evidence& result, const char* backend,
                  const char* supervision_cause) {
    std::printf("{\"event\":\"result\",\"trial\":%s,\"transport\":%s,\"mode\":%s,"
        "\"measured_count\":%zu,\"confirmed_body_bytes\":%llu,\"server_cpu_ns\":%llu,"
        "\"success\":%s,\"probe_success\":%s,\"body_bytes\":%zu,\"stream_block_bytes\":%zu,"
        "\"warmup_count\":%u,\"workers\":1,\"backend\":%s,\"tls_version\":%s,\"tls_cipher\":%s,"
        "\"openssl_version\":%s,\"build_type\":%s,\"build_head\":%s,\"build_dirty\":%s,\"compiler\":%s,\"performance_eligible\":false,\"responses\":[",
        json_quote(config.trial).c_str(), json_quote(config.transport).c_str(), json_quote(config.mode).c_str(),
        result.measured_count, static_cast<unsigned long long>(result.confirmed),
        static_cast<unsigned long long>(result.cpu_ns), result.success ? "true" : "false",
        result.probe_success ? "true" : "false", body_bytes, block_bytes,
        result.response_count && result.responses[0].sent.success() ? 1u : 0u,
        json_quote(backend).c_str(), json_quote(result.tls_version).c_str(), json_quote(result.tls_cipher).c_str(),
        json_quote(OpenSSL_version(OPENSSL_VERSION)).c_str(), json_quote(ELIO_HTTP_METRICS_BUILD_TYPE).c_str(),
        json_quote(ELIO_HTTP_METRICS_BUILD_HEAD).c_str(), ELIO_HTTP_METRICS_BUILD_DIRTY ? "true" : "false",
        json_quote(__VERSION__).c_str());
    for (size_t i = 0; i < result.response_count; ++i) {
        const auto& row = result.responses[i];
        std::printf("%s{\"phase\":%s,\"sequence\":%zu,\"success\":%s,\"reusable\":%s,"
            "\"confirmed_body_bytes\":%llu,\"server_cpu_ns\":%llu,\"error\":%d,\"transport_error\":%d}",
            i ? "," : "", json_quote(row.phase).c_str(), row.sequence,
            row.sent.success() ? "true" : "false", row.sent.reusable ? "true" : "false",
            static_cast<unsigned long long>(row.sent.result.confirmed_body_bytes),
            static_cast<unsigned long long>(row.cpu_ns), static_cast<int>(row.sent.result.error), row.sent.result.transport_error);
    }
    std::fputs("]", stdout);
    if (!result.success) {
        std::printf(",\"failure\":{\"stage\":%s,\"error_type\":%s,\"error\":%s,"
            "\"supervision_cause\":%s,\"request_phase\":%s,\"request_sequence\":%zu,"
            "\"request_bytes\":%zu,\"request_complete\":%s,\"request_elapsed_ns\":%lld,\"read_result\":",
            json_quote(result.stage).c_str(), json_quote(result.error_type).c_str(),
            json_quote(result.error).c_str(), json_quote(supervision_cause).c_str(),
            json_quote(result.request_phase).c_str(), result.request_sequence, result.request_bytes,
            result.request_complete ? "true" : "false", static_cast<long long>(result.request_elapsed_ns));
        if (result.read_result_available)
            std::printf("{\"result\":%d,\"flags\":%u}", result.last_read.result,
                        static_cast<unsigned>(result.last_read.flags));
        else std::fputs("null", stdout);
        std::fputs("}", stdout);
    }
    std::puts("}");
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto config = parse_options(argc, argv);
        log::logger::instance().set_level(log::level::error);
        std::unique_ptr<tls::tls_context> tls_context;
        if (config.transport == "tls") {
            tls_context = std::make_unique<tls::tls_context>(tls::tls_mode::server);
            if (!tls_context->load_certificate(config.certificate) || !tls_context->load_private_key(config.key)) {
                throw std::runtime_error("failed to load TLS credentials");
            }
        }
        const std::string payload(body_bytes, 'x');
        reply complete = response::ok(payload);
        auto listener = net::tcp_listener::bind(net::ipv4_address("127.0.0.1", 0));
        if (!listener) throw std::runtime_error("bind failed");
        runtime::scheduler scheduler(1);
        supervision control;
        evidence result;
        std::promise<void> done;
        auto finished = done.get_future();
        scheduler.start();
        const auto* backend = scheduler.get_worker(0)->io_context().get_backend_name();
        control.start_bounded_phase();
        scheduler.go([&]() -> coro::task<void> {
            try {
                co_await run_connection(*listener, config, payload, complete, tls_context.get(), control, result);
                done.set_value();
            } catch (...) { done.set_exception(std::current_exception()); }
        });
        std::printf("{\"event\":\"ready\",\"port\":%u,\"backend\":%s,\"workers\":1}\n",
            static_cast<unsigned>(listener->local_address().port()), json_quote(backend).c_str());
        std::fflush(stdout);
        const auto total_deadline = std::chrono::steady_clock::now() + 60s;
        bool expired = false;
        const char* supervision_cause = "none";
        while (finished.wait_for(5ms) != std::future_status::ready) {
            const auto phase_deadline = control.phase_deadline.load();
            if (std::chrono::steady_clock::now() >= total_deadline ||
                (phase_deadline && monotonic_ns() >= phase_deadline)) {
                expired = true;
                supervision_cause = std::chrono::steady_clock::now() >= total_deadline
                    ? "trial_deadline" : "phase_deadline";
                control.stop.cancel();
                break;
            }
        }
        if (finished.wait_for(10s) != std::future_status::ready) {
            // Never unwind live coroutine/borrowed state after failed cleanup.
            // The enclosing driver retains this explicit failure and process exit.
            std::fputs("metrics fixture cancellation cleanup exceeded budget\n", stderr);
            std::fflush(stderr);
            // supervision_cause is one of the fixed literals above. Keep this
            // emergency record allocation-free before terminating live work.
            std::printf("{\"event\":\"failure\",\"error_type\":\"cleanup_timeout\",\"supervision_cause\":\"%s\"}\n",
                        supervision_cause);
            std::fflush(stdout);
            std::_Exit(3);
        }
        try { finished.get(); }
        catch (const std::bad_alloc& error) {
            std::fprintf(stderr, "metrics fixture: %s\n", error.what());
            result.success = false;
            result.error = error.what();
            result.error_type = "std::bad_alloc";
        }
        catch (const std::exception& error) {
            std::fprintf(stderr, "metrics fixture: %s\n", error.what());
            result.success = false;
            result.error = error.what();
            result.error_type = "std::exception";
        }
        catch (...) { result.success = false; result.error_type = "unknown_exception"; }
        if (expired) result.success = false;
        if (!result.request_complete && !result.read_result_available && result.request_started_ns)
            result.request_elapsed_ns = monotonic_ns() - result.request_started_ns;
        if (!scheduler.shutdown(10s)) {
            std::fputs("metrics fixture scheduler failed to drain\n", stderr);
            std::fflush(stderr);
            std::printf("{\"event\":\"failure\",\"error_type\":\"scheduler_cleanup_timeout\",\"supervision_cause\":\"%s\"}\n",
                        supervision_cause);
            std::fflush(stdout);
            std::_Exit(4);
        }
        print_result(config, result, backend, supervision_cause);
        if (result.success) std::puts("{\"event\":\"stopped\"}");
        return result.success ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "metrics fixture setup: %s\n", error.what());
        return 2;
    }
}
