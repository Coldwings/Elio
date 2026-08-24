/// @file bench_tcp_libuv_server.cpp
/// @brief Protocol-validating libuv TCP benchmark server.

#include <uv.h>

#include "bench_tcp_server_protocol.hpp"

#include <cstdio>
#include <memory>
#include <span>
#include <vector>

namespace {

struct connection {
    connection() : evidence("libuv") {}

    uv_tcp_t socket{};
    uv_write_t write{};
    std::vector<uint8_t> record;
    std::size_t used = 0;
    std::size_t expected = bench::kRecordHeaderBytes;
    bench::server_protocol protocol;
    bench::server_connection_evidence evidence;
};

struct listener_state {
    uv_tcp_t socket{};
    std::size_t maximum_record_size = 0;
};

void close_connection(connection* state) {
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&state->socket))) {
        uv_read_stop(reinterpret_cast<uv_stream_t*>(&state->socket));
        uv_close(reinterpret_cast<uv_handle_t*>(&state->socket),
                 [](uv_handle_t* handle) {
                     delete static_cast<connection*>(handle->data);
                 });
    }
}

void alloc_record(uv_handle_t* handle, std::size_t, uv_buf_t* buffer) {
    auto* state = static_cast<connection*>(handle->data);
    buffer->base = reinterpret_cast<char*>(state->record.data() + state->used);
    buffer->len = state->expected - state->used;
}

void start_read(connection* state);

void write_complete(uv_write_t* request, int status) {
    auto* state = static_cast<connection*>(request->data);
    if (status < 0) {
        state->evidence.transport_error();
        close_connection(state);
        return;
    }
    state->evidence.complete_write(state->expected);
    state->used = 0;
    state->expected = bench::kRecordHeaderBytes;
    start_read(state);
}

void validate_and_write(connection* state) {
    const auto bytes =
        std::span<const uint8_t>(state->record.data(), state->expected);
    if (!state->protocol.accept(bytes)) {
        state->evidence.integrity_error();
        close_connection(state);
        return;
    }
    state->evidence.verify_record(bytes.size());

    uv_read_stop(reinterpret_cast<uv_stream_t*>(&state->socket));
    state->write.data = state;
    state->evidence.begin_write(bytes.size());
    uv_buf_t buffer = uv_buf_init(
        reinterpret_cast<char*>(state->record.data()),
        static_cast<unsigned int>(state->expected));
    const int status = uv_write(
        &state->write, reinterpret_cast<uv_stream_t*>(&state->socket),
        &buffer, 1, write_complete);
    if (status < 0) {
        state->evidence.transport_error();
        close_connection(state);
    }
}

void read_record(uv_stream_t* stream, ssize_t count, const uv_buf_t*) {
    auto* state = static_cast<connection*>(stream->data);
    if (count < 0) {
        if (state->used != 0) state->evidence.transport_error();
        close_connection(state);
        return;
    }
    if (count == 0) {
        return;
    }

    state->used += static_cast<std::size_t>(count);
    if (state->used != state->expected) {
        return;
    }

    if (state->expected == bench::kRecordHeaderBytes) {
        const auto header = bench::decode_record_header(
            std::span<const uint8_t>(state->record.data(), state->expected));
        if (!bench::valid_record_size(header.size, state->record.size())) {
            state->evidence.integrity_error();
            close_connection(state);
            return;
        }
        state->expected = header.size;
        if (state->used != state->expected) {
            return;
        }
    }
    state->evidence.receive_record(std::span<const uint8_t>(
        state->record.data(), state->expected));
    validate_and_write(state);
}

void start_read(connection* state) {
    const int status = uv_read_start(
        reinterpret_cast<uv_stream_t*>(&state->socket), alloc_record,
        read_record);
    if (status < 0) {
        close_connection(state);
    }
}

void accept_connection(uv_stream_t* listener, int status) {
    if (status < 0) {
        return;
    }
    auto* server = static_cast<listener_state*>(listener->data);
    auto state = std::make_unique<connection>();
    state->record.resize(server->maximum_record_size);
    if (uv_tcp_init(listener->loop, &state->socket) < 0) {
        return;
    }
    state->socket.data = state.get();
    if (uv_accept(listener,
                  reinterpret_cast<uv_stream_t*>(&state->socket)) < 0) {
        uv_close(reinterpret_cast<uv_handle_t*>(&state->socket),
                 [](uv_handle_t* handle) {
                     delete static_cast<connection*>(handle->data);
                 });
        state.release();
        return;
    }

    (void)uv_tcp_nodelay(&state->socket, 1);
    auto* accepted = state.release();
    start_read(accepted);
}

} // namespace

int main(int argc, char* argv[]) {
    bench::config cfg;
    try {
        cfg = bench::parse_args(argc, argv, "libuv server");
    } catch (const bench::argument_error&) {
        return 2;
    }

    uv_loop_t loop;
    if (uv_loop_init(&loop) < 0) {
        return 1;
    }
    listener_state server;
    server.maximum_record_size = bench::server_max_record_size(cfg);
    int status = uv_tcp_init(&loop, &server.socket);
    sockaddr_in address{};
    if (status == 0) {
        status = uv_ip4_addr("127.0.0.1", cfg.port, &address);
    }
    if (status == 0) {
        status = uv_tcp_bind(
            &server.socket, reinterpret_cast<const sockaddr*>(&address), 0);
    }
    server.socket.data = &server;
    if (status == 0) {
        status = uv_listen(reinterpret_cast<uv_stream_t*>(&server.socket), 128,
                           accept_connection);
    }
    if (status < 0) {
        std::fprintf(stderr, "libuv benchmark server failed: %s\n",
                     uv_strerror(status));
        uv_close(reinterpret_cast<uv_handle_t*>(&server.socket), nullptr);
        uv_run(&loop, UV_RUN_DEFAULT);
        uv_loop_close(&loop);
        return 1;
    }

    std::printf("libuv benchmark server listening on 127.0.0.1:%u\n",
                static_cast<unsigned>(cfg.port));
    std::fflush(stdout);
    uv_run(&loop, UV_RUN_DEFAULT);
    uv_loop_close(&loop);
    return 0;
}
