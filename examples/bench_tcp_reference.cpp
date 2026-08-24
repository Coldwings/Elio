/// @file bench_tcp_reference.cpp
/// @brief Protocol-validating POSIX peer for TCP client comparisons.

#include "bench_tcp_server_protocol.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

volatile std::sig_atomic_t running = 1;

void stop_server(int) { running = 0; }

bool set_flag(int fd, int level, int option, int value) {
    return ::setsockopt(fd, level, option, &value, sizeof(value)) == 0;
}

bool send_all(int fd, const unsigned char* data, std::size_t size) {
    while (size != 0) {
        const ssize_t sent = ::send(fd, data, size, MSG_NOSIGNAL);
        if (sent > 0) {
            data += sent;
            size -= static_cast<std::size_t>(sent);
        } else if (sent < 0 && errno == EINTR) {
            if (!running) return false;
        } else {
            return false;
        }
    }
    return true;
}

enum class receive_status { complete, clean_eof, error };

receive_status receive_all(int fd, unsigned char* data, std::size_t size,
                           bool allow_initial_eof) {
    const std::size_t requested = size;
    while (size != 0) {
        const ssize_t received = ::recv(fd, data, size, 0);
        if (received > 0) {
            data += received;
            size -= static_cast<std::size_t>(received);
        } else if (received == 0) {
            return allow_initial_eof && size == requested
                ? receive_status::clean_eof : receive_status::error;
        } else if (errno == EINTR) {
            if (!running) return receive_status::error;
        } else {
            return receive_status::error;
        }
    }
    return receive_status::complete;
}

void echo_connection(int fd, std::vector<unsigned char>& record) {
    const int enabled = 1;
    (void)set_flag(fd, IPPROTO_TCP, TCP_NODELAY, enabled);
    bench::server_protocol protocol;
    bench::server_connection_evidence evidence("posix-reference");
    while (running) {
        const auto header_status = receive_all(
            fd, record.data(), bench::kRecordHeaderBytes, true);
        if (header_status == receive_status::clean_eof) {
            // A readiness probe is an ordinary connection with no payload.
            return;
        }
        if (header_status != receive_status::complete) {
            evidence.transport_error();
            return;
        }
        const auto header = bench::decode_record_header(
            std::span<const uint8_t>(record.data(), bench::kRecordHeaderBytes));
        if (!bench::valid_record_size(header.size, record.size())) {
            evidence.integrity_error();
            return;
        }
        const std::size_t remainder = header.size - bench::kRecordHeaderBytes;
        if (remainder != 0 &&
            receive_all(fd, record.data() + bench::kRecordHeaderBytes,
                        remainder, false) != receive_status::complete) {
            evidence.transport_error();
            return;
        }
        const auto bytes =
            std::span<const uint8_t>(record.data(), header.size);
        evidence.receive_record(bytes);
        if (!protocol.accept(bytes)) {
            evidence.integrity_error();
            return;
        }
        evidence.verify_record(bytes.size());
        evidence.begin_write(bytes.size());
        if (!send_all(fd, record.data(), header.size)) {
            evidence.transport_error();
            return;
        }
        evidence.complete_write(bytes.size());
    }
}

} // namespace

int main(int argc, char* argv[]) {
    bench::config cfg;
    try {
        cfg = bench::parse_args(argc, argv, "POSIX reference server");
    } catch (const bench::argument_error&) {
        return 2;
    }

    struct sigaction action {};
    action.sa_handler = stop_server;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    (void)::sigaction(SIGINT, &action, nullptr);
    (void)::sigaction(SIGTERM, &action, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        std::fprintf(stderr, "socket failed: %s\n", std::strerror(errno));
        return 1;
    }
    const int enabled = 1;
    if (!set_flag(listener, SOL_SOCKET, SO_REUSEADDR, enabled)) {
        std::fprintf(stderr, "SO_REUSEADDR failed: %s\n", std::strerror(errno));
        ::close(listener);
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(cfg.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0 || ::listen(listener, 128) != 0) {
        std::fprintf(stderr, "listen on 127.0.0.1:%u failed: %s\n",
                     static_cast<unsigned>(cfg.port), std::strerror(errno));
        ::close(listener);
        return 1;
    }

    std::printf("POSIX reference server listening on 127.0.0.1:%u\n",
                static_cast<unsigned>(cfg.port));
    std::fflush(stdout);

    // Storage is allocated once and reused across all sequential clients.
    std::vector<unsigned char> record(bench::server_max_record_size(cfg));
    while (running) {
        const int client = ::accept(listener, nullptr, nullptr);
        if (client >= 0) {
            echo_connection(client, record);
            ::close(client);
        } else if (errno != EINTR) {
            std::fprintf(stderr, "accept failed: %s\n", std::strerror(errno));
        }
    }
    ::close(listener);
    return 0;
}
