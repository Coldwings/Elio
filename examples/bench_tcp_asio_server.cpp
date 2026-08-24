/// @file bench_tcp_asio_server.cpp
/// @brief Protocol-validating standalone Asio TCP benchmark server.

#include <asio.hpp>

#include "bench_tcp_server_protocol.hpp"

#include <cstdio>
#include <memory>
#include <span>
#include <vector>

namespace {

using asio::ip::tcp;

class session : public std::enable_shared_from_this<session> {
public:
    session(tcp::socket socket, std::size_t maximum_record_size)
        : socket_(std::move(socket)), record_(maximum_record_size),
          evidence_("asio") {}

    void start() {
        asio::error_code error;
        socket_.set_option(tcp::no_delay(true), error);
        read_header();
    }

private:
    void read_header() {
        auto self = shared_from_this();
        asio::async_read(
            socket_, asio::buffer(record_.data(), bench::kRecordHeaderBytes),
            [self](const asio::error_code& error, std::size_t transferred) {
                if (error || transferred != bench::kRecordHeaderBytes) {
                    if (transferred != 0) self->evidence_.transport_error();
                    return;
                }
                self->on_header();
            });
    }

    void on_header() {
        const auto header = bench::decode_record_header(
            std::span<const uint8_t>(record_.data(), bench::kRecordHeaderBytes));
        if (!bench::valid_record_size(header.size, record_.size())) {
            evidence_.integrity_error();
            return;
        }
        record_size_ = header.size;
        const std::size_t remainder = record_size_ - bench::kRecordHeaderBytes;
        if (remainder == 0) {
            validate_and_write();
            return;
        }

        auto self = shared_from_this();
        asio::async_read(
            socket_,
            asio::buffer(record_.data() + bench::kRecordHeaderBytes, remainder),
            [self, remainder](const asio::error_code& error,
                              std::size_t transferred) {
                if (error || transferred != remainder) {
                    self->evidence_.transport_error();
                    return;
                }
                self->validate_and_write();
            });
    }

    void validate_and_write() {
        const auto bytes =
            std::span<const uint8_t>(record_.data(), record_size_);
        evidence_.receive_record(bytes);
        if (!protocol_.accept(bytes)) {
            evidence_.integrity_error();
            return;
        }
        evidence_.verify_record(bytes.size());

        auto self = shared_from_this();
        evidence_.begin_write(bytes.size());
        asio::async_write(
            socket_, asio::buffer(record_.data(), record_size_),
            [self](const asio::error_code& error, std::size_t transferred) {
                if (error || transferred != self->record_size_) {
                    self->evidence_.transport_error();
                    return;
                }
                self->evidence_.complete_write(transferred);
                self->read_header();
            });
    }

    tcp::socket socket_;
    std::vector<uint8_t> record_;
    std::size_t record_size_ = 0;
    bench::server_protocol protocol_;
    bench::server_connection_evidence evidence_;
};

class server {
public:
    server(asio::io_context& context, uint16_t port,
           std::size_t maximum_record_size)
        : acceptor_(context), maximum_record_size_(maximum_record_size) {
        const tcp::endpoint endpoint(asio::ip::address_v4::loopback(), port);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen(128);
    }

    void start() { accept(); }

private:
    void accept() {
        acceptor_.async_accept(
            [this](const asio::error_code& error, tcp::socket socket) {
                if (!error) {
                    std::make_shared<session>(
                        std::move(socket), maximum_record_size_)->start();
                }
                accept();
            });
    }

    tcp::acceptor acceptor_;
    std::size_t maximum_record_size_;
};

} // namespace

int main(int argc, char* argv[]) {
    bench::config cfg;
    try {
        cfg = bench::parse_args(argc, argv, "Asio server");
    } catch (const bench::argument_error&) {
        return 2;
    }

    try {
        asio::io_context context(1);
        server peer(context, cfg.port, bench::server_max_record_size(cfg));
        peer.start();
        std::printf("Asio benchmark server listening on 127.0.0.1:%u\n",
                    static_cast<unsigned>(cfg.port));
        std::fflush(stdout);
        context.run();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Asio benchmark server failed: %s\n", error.what());
        return 1;
    }
}
