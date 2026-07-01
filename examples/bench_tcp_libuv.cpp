/// @file bench_tcp_libuv.cpp
/// @brief TCP Loopback Benchmark using libuv (callback-based)
///
/// Server and client for measuring TCP ping-pong latency and streaming
/// throughput on loopback. Compare with bench_tcp_elio and bench_tcp_asio.
///
/// Usage: bench_tcp_libuv -s  (server)
///        bench_tcp_libuv -c  (client, default)

#include <uv.h>
#include "bench_tcp_common.hpp"

#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

struct server_conn {
    uv_tcp_t handle;
    char     buf[65536];
};

struct echo_write_req {
    uv_write_t    req;
    std::vector<char> data;
};

static void server_alloc_cb(uv_handle_t* handle, size_t /*suggested*/,
                            uv_buf_t* buf) {
    auto* conn = static_cast<server_conn*>(handle->data);
    buf->base = conn->buf;
    buf->len  = sizeof(conn->buf);
}

static void server_write_cb(uv_write_t* req, int /*status*/) {
    delete reinterpret_cast<echo_write_req*>(req);
}

static void server_close_cb(uv_handle_t* handle) {
    delete static_cast<server_conn*>(handle->data);
}

static void server_read_cb(uv_stream_t* stream, ssize_t nread,
                           const uv_buf_t* buf) {
    if (nread < 0) {
        uv_close(reinterpret_cast<uv_handle_t*>(stream), server_close_cb);
        return;
    }

    auto* wreq = new echo_write_req;
    wreq->data.assign(buf->base, buf->base + nread);
    uv_buf_t wbuf = uv_buf_init(wreq->data.data(),
                                static_cast<unsigned int>(nread));
    uv_write(&wreq->req, stream, &wbuf, 1, server_write_cb);
}

static void server_on_connection(uv_stream_t* server, int status) {
    if (status < 0) return;

    auto* conn = new server_conn;
    conn->handle.data = conn;
    uv_tcp_init(server->loop, &conn->handle);

    if (uv_accept(server, reinterpret_cast<uv_stream_t*>(&conn->handle)) == 0) {
        uv_tcp_nodelay(&conn->handle, 1);
        uv_read_start(reinterpret_cast<uv_stream_t*>(&conn->handle),
                      server_alloc_cb, server_read_cb);
    } else {
        uv_close(reinterpret_cast<uv_handle_t*>(&conn->handle), server_close_cb);
    }
}

static int run_server(const bench::config& cfg) {
    uv_loop_t loop;
    uv_loop_init(&loop);

    uv_tcp_t server;
    uv_tcp_init(&loop, &server);

    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", cfg.port, &addr);
    uv_tcp_bind(&server, reinterpret_cast<const struct sockaddr*>(&addr), 0);
    uv_tcp_nodelay(&server, 1);

    int r = uv_listen(reinterpret_cast<uv_stream_t*>(&server), 128,
                      server_on_connection);
    if (r) {
        std::fprintf(stderr, "Listen error: %s\n", uv_strerror(r));
        return 1;
    }

    std::printf("libuv server listening on port %d\n", cfg.port);
    std::fflush(stdout);
    uv_run(&loop, UV_RUN_DEFAULT);
    uv_loop_close(&loop);
    return 0;
}

// ---------------------------------------------------------------------------
// Client: shared write request
// ---------------------------------------------------------------------------

struct write_req {
    uv_write_t    req;
    std::vector<char> data;
    uv_buf_t buf;
};

// ---------------------------------------------------------------------------
// Client: ping-pong
// ---------------------------------------------------------------------------

struct pp_client {
    uv_tcp_t    tcp;
    uv_timer_t  warmup_timer;
    uv_timer_t  measure_timer;
    uv_connect_t connect_req;

    std::vector<char> send_buf;
    std::vector<char> recv_buf;
    size_t msg_size   = 0;
    size_t recv_offset = 0;
    bool measuring    = false;
    bool done         = false;

    std::chrono::steady_clock::time_point t0{};
    std::vector<uint64_t> latencies;
};

static void pp_write_cb(uv_write_t* req, int /*status*/) {
    delete reinterpret_cast<write_req*>(req);
}

static void pp_alloc_cb(uv_handle_t* handle, size_t /*suggested*/,
                        uv_buf_t* buf) {
    auto* c = static_cast<pp_client*>(handle->data);
    buf->base = c->recv_buf.data() + c->recv_offset;
    buf->len  = static_cast<unsigned int>(c->msg_size - c->recv_offset);
}

static void pp_close_cb(uv_handle_t* /*handle*/) {}

static void pp_send(pp_client* c) {
    c->t0 = std::chrono::steady_clock::now();
    auto* wreq = new write_req;
    wreq->data = c->send_buf;
    wreq->buf = uv_buf_init(wreq->data.data(),
                            static_cast<unsigned int>(c->msg_size));
    uv_write(&wreq->req, reinterpret_cast<uv_stream_t*>(&c->tcp),
             &wreq->buf, 1, pp_write_cb);
}

static void pp_read_cb(uv_stream_t* stream, ssize_t nread,
                       const uv_buf_t* /*buf*/) {
    auto* c = static_cast<pp_client*>(stream->data);
    if (nread < 0 || c->done) {
        if (!c->done) {
            c->done = true;
            uv_read_stop(stream);
            uv_close(reinterpret_cast<uv_handle_t*>(&c->tcp), pp_close_cb);
            uv_close(reinterpret_cast<uv_handle_t*>(&c->warmup_timer), pp_close_cb);
            uv_close(reinterpret_cast<uv_handle_t*>(&c->measure_timer), pp_close_cb);
        }
        return;
    }

    c->recv_offset += nread;
    if (c->recv_offset < c->msg_size) return;

    auto now = std::chrono::steady_clock::now();
    if (c->measuring) {
        auto rtt = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - c->t0).count();
        c->latencies.push_back(rtt);
    }

    c->recv_offset = 0;
    pp_send(c);
}

static void pp_warmup_timer_cb(uv_timer_t* timer) {
    auto* c = static_cast<pp_client*>(timer->data);
    c->measuring = true;
    c->latencies.clear();
    uv_timer_stop(timer);
}

static void pp_measure_timer_cb(uv_timer_t* timer) {
    auto* c = static_cast<pp_client*>(timer->data);
    c->done = true;
    uv_read_stop(reinterpret_cast<uv_stream_t*>(&c->tcp));
    uv_close(reinterpret_cast<uv_handle_t*>(&c->tcp), pp_close_cb);
    uv_close(reinterpret_cast<uv_handle_t*>(&c->warmup_timer), pp_close_cb);
    uv_close(reinterpret_cast<uv_handle_t*>(&c->measure_timer), pp_close_cb);
}

static void pp_connect_cb(uv_connect_t* req, int status) {
    auto* c = static_cast<pp_client*>(req->data);
    if (status < 0) {
        std::fprintf(stderr, "Connect error: %s\n", uv_strerror(status));
        c->done = true;
        uv_close(reinterpret_cast<uv_handle_t*>(&c->tcp), pp_close_cb);
        uv_close(reinterpret_cast<uv_handle_t*>(&c->warmup_timer), pp_close_cb);
        uv_close(reinterpret_cast<uv_handle_t*>(&c->measure_timer), pp_close_cb);
        return;
    }

    uv_tcp_nodelay(&c->tcp, 1);
    c->tcp.data = c;

    uv_read_start(reinterpret_cast<uv_stream_t*>(&c->tcp),
                  pp_alloc_cb, pp_read_cb);
    pp_send(c);
}

static bench::pingpong_stats run_client_pingpong(const bench::config& cfg,
                                                  size_t msg_size) {
    uv_loop_t loop;
    uv_loop_init(&loop);

    pp_client c;
    c.msg_size = msg_size;
    c.send_buf.assign(msg_size, 'X');
    c.recv_buf.resize(msg_size);

    uv_tcp_init(&loop, &c.tcp);
    c.tcp.data = &c;

    uv_timer_init(&loop, &c.warmup_timer);
    c.warmup_timer.data = &c;
    uv_timer_start(&c.warmup_timer, pp_warmup_timer_cb,
                   cfg.warmup_s * 1000, 0);

    uv_timer_init(&loop, &c.measure_timer);
    c.measure_timer.data = &c;
    uv_timer_start(&c.measure_timer, pp_measure_timer_cb,
                   (cfg.warmup_s + cfg.duration_s) * 1000, 0);

    c.connect_req.data = &c;
    struct sockaddr_in addr;
    uv_ip4_addr(cfg.host.c_str(), cfg.port, &addr);
    uv_tcp_connect(&c.connect_req, &c.tcp,
                   reinterpret_cast<const struct sockaddr*>(&addr),
                   pp_connect_cb);

    uv_run(&loop, UV_RUN_DEFAULT);

    auto stats = bench::pingpong_stats::compute(
        c.latencies, static_cast<double>(cfg.duration_s));

    uv_loop_close(&loop);
    return stats;
}

// ---------------------------------------------------------------------------
// Client: streaming
// ---------------------------------------------------------------------------

struct st_client {
    uv_tcp_t    tcp;
    uv_timer_t  warmup_timer;
    uv_timer_t  measure_timer;
    uv_connect_t connect_req;

    std::vector<char> send_buf;
    std::vector<char> recv_buf;
    size_t msg_size   = 0;
    size_t recv_offset = 0;
    int pipeline_depth = 16;
    bool measuring    = false;
    bool done         = false;

    std::atomic<uint64_t> total_bytes{0};
    std::atomic<uint64_t> total_msgs{0};
};

static void st_write_cb(uv_write_t* req, int status) {
    auto* wreq = reinterpret_cast<write_req*>(req);
    auto* c = static_cast<st_client*>(wreq->req.data);
    delete wreq;
    if (status < 0 || c->done) return;
    // Send next to keep pipeline full
    auto* nw = new write_req;
    nw->data = c->send_buf;
    nw->req.data = c;
    nw->buf = uv_buf_init(nw->data.data(),
                          static_cast<unsigned int>(c->msg_size));
    uv_write(&nw->req, reinterpret_cast<uv_stream_t*>(&c->tcp),
             &nw->buf, 1, st_write_cb);
}

static void st_alloc_cb(uv_handle_t* handle, size_t /*suggested*/,
                        uv_buf_t* buf) {
    auto* c = static_cast<st_client*>(handle->data);
    buf->base = c->recv_buf.data() + c->recv_offset;
    buf->len  = static_cast<unsigned int>(c->recv_buf.size() - c->recv_offset);
}

static void st_close_cb(uv_handle_t* /*handle*/) {}

static void st_read_cb(uv_stream_t* stream, ssize_t nread,
                       const uv_buf_t* /*buf*/) {
    auto* c = static_cast<st_client*>(stream->data);
    if (nread < 0 || c->done) {
        if (!c->done) {
            c->done = true;
            uv_read_stop(stream);
            uv_close(reinterpret_cast<uv_handle_t*>(&c->tcp), st_close_cb);
            uv_close(reinterpret_cast<uv_handle_t*>(&c->warmup_timer), st_close_cb);
            uv_close(reinterpret_cast<uv_handle_t*>(&c->measure_timer), st_close_cb);
        }
        return;
    }

    size_t total = c->recv_offset + static_cast<size_t>(nread);
    const char* ptr = c->recv_buf.data();
    while (total >= c->msg_size) {
        if (c->measuring) {
            c->total_bytes.fetch_add(c->msg_size, std::memory_order_relaxed);
            c->total_msgs.fetch_add(1, std::memory_order_relaxed);
        }
        ptr += c->msg_size;
        total -= c->msg_size;
    }

    c->recv_offset = total;
    if (total > 0) {
        std::memmove(c->recv_buf.data(), ptr, total);
    }
}

static void st_warmup_timer_cb(uv_timer_t* timer) {
    auto* c = static_cast<st_client*>(timer->data);
    c->measuring = true;
    c->total_bytes.store(0, std::memory_order_release);
    c->total_msgs.store(0, std::memory_order_release);
    uv_timer_stop(timer);
}

static void st_measure_timer_cb(uv_timer_t* timer) {
    auto* c = static_cast<st_client*>(timer->data);
    c->done = true;
    uv_read_stop(reinterpret_cast<uv_stream_t*>(&c->tcp));
    uv_close(reinterpret_cast<uv_handle_t*>(&c->tcp), st_close_cb);
    uv_close(reinterpret_cast<uv_handle_t*>(&c->warmup_timer), st_close_cb);
    uv_close(reinterpret_cast<uv_handle_t*>(&c->measure_timer), st_close_cb);
}

static void st_connect_cb(uv_connect_t* req, int status) {
    auto* c = static_cast<st_client*>(req->data);
    if (status < 0) {
        std::fprintf(stderr, "Connect error: %s\n", uv_strerror(status));
        c->done = true;
        uv_close(reinterpret_cast<uv_handle_t*>(&c->tcp), st_close_cb);
        uv_close(reinterpret_cast<uv_handle_t*>(&c->warmup_timer), st_close_cb);
        uv_close(reinterpret_cast<uv_handle_t*>(&c->measure_timer), st_close_cb);
        return;
    }

    uv_tcp_nodelay(&c->tcp, 1);
    c->tcp.data = c;

    uv_read_start(reinterpret_cast<uv_stream_t*>(&c->tcp),
                  st_alloc_cb, st_read_cb);

    // Kick off pipeline_depth concurrent writes
    for (int i = 0; i < c->pipeline_depth; ++i) {
        auto* wreq = new write_req;
        wreq->data = c->send_buf;
        wreq->req.data = c;
        wreq->buf = uv_buf_init(wreq->data.data(),
                                static_cast<unsigned int>(c->msg_size));
        uv_write(&wreq->req, reinterpret_cast<uv_stream_t*>(&c->tcp),
                 &wreq->buf, 1, st_write_cb);
    }
}

static bench::streaming_stats run_client_streaming(const bench::config& cfg,
                                                    size_t msg_size) {
    uv_loop_t loop;
    uv_loop_init(&loop);

    st_client c;
    c.msg_size = msg_size;
    c.send_buf.assign(msg_size, 'X');
    c.recv_buf.resize(65536);
    c.pipeline_depth = std::min(cfg.pipeline_depth, 64);

    uv_tcp_init(&loop, &c.tcp);
    c.tcp.data = &c;

    uv_timer_init(&loop, &c.warmup_timer);
    c.warmup_timer.data = &c;
    uv_timer_start(&c.warmup_timer, st_warmup_timer_cb,
                   cfg.warmup_s * 1000, 0);

    uv_timer_init(&loop, &c.measure_timer);
    c.measure_timer.data = &c;
    uv_timer_start(&c.measure_timer, st_measure_timer_cb,
                   (cfg.warmup_s + cfg.duration_s) * 1000, 0);

    c.connect_req.data = &c;
    struct sockaddr_in addr;
    uv_ip4_addr(cfg.host.c_str(), cfg.port, &addr);
    uv_tcp_connect(&c.connect_req, &c.tcp,
                   reinterpret_cast<const struct sockaddr*>(&addr),
                   st_connect_cb);

    uv_run(&loop, UV_RUN_DEFAULT);

    auto stats = bench::streaming_stats::compute(
        c.total_bytes.load(std::memory_order_acquire),
        c.total_msgs.load(std::memory_order_acquire),
        static_cast<double>(cfg.duration_s));

    uv_loop_close(&loop);
    return stats;
}

// ---------------------------------------------------------------------------
// Client: drive all message sizes
// ---------------------------------------------------------------------------

static void run_client(const bench::config& cfg) {
    bench::print_header("libuv", cfg);

    bool do_pingpong  = (cfg.type == bench::config::test_type::pingpong ||
                         cfg.type == bench::config::test_type::both);
    bool do_streaming = (cfg.type == bench::config::test_type::streaming ||
                         cfg.type == bench::config::test_type::both);

    if (do_pingpong) {
        bench::print_pingpong_table_header();
        for (int i = 0; i < bench::kNumMessageSizes; ++i) {
            size_t sz = bench::kMessageSizes[i];
            auto stats = run_client_pingpong(cfg, sz);
            stats.print_row(sz);
            std::fflush(stdout);
        }
        std::printf("\n");
    }

    if (do_streaming) {
        bench::print_streaming_table_header();
        for (int i = 0; i < bench::kNumMessageSizes; ++i) {
            size_t sz = bench::kMessageSizes[i];
            auto stats = run_client_streaming(cfg, sz);
            stats.print_row(sz);
            std::fflush(stdout);
        }
        std::printf("\n");
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    auto cfg = bench::parse_args(argc, argv, "libuv");

    if (cfg.run_mode == bench::config::mode::server) {
        return run_server(cfg);
    } else {
        run_client(cfg);
        return 0;
    }
}
