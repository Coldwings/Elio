# Example programs

if(ELIO_ENABLE_DEVELOPER_WARNINGS)
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(ELIO_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

# Static linking for libgcc and libstdc++ for portability
set(STATIC_LINK_FLAGS -static-libgcc -static-libstdc++)

add_executable(hello_world hello_world.cpp)
target_link_libraries(hello_world PRIVATE elio)
target_link_options(hello_world PRIVATE ${STATIC_LINK_FLAGS})

add_executable(chained_coroutines chained_coroutines.cpp)
target_link_libraries(chained_coroutines PRIVATE elio)
target_link_options(chained_coroutines PRIVATE ${STATIC_LINK_FLAGS})

add_executable(exception_handling exception_handling.cpp)
target_link_libraries(exception_handling PRIVATE elio)
target_link_options(exception_handling PRIVATE ${STATIC_LINK_FLAGS})

add_executable(parallel_tasks parallel_tasks.cpp)
target_link_libraries(parallel_tasks PRIVATE elio)
target_link_options(parallel_tasks PRIVATE ${STATIC_LINK_FLAGS})

add_executable(dynamic_threads dynamic_threads.cpp)
target_link_libraries(dynamic_threads PRIVATE elio)
target_link_options(dynamic_threads PRIVATE ${STATIC_LINK_FLAGS})

add_executable(autoscaler_example autoscaler_example.cpp)
target_link_libraries(autoscaler_example PRIVATE elio)
target_link_options(autoscaler_example PRIVATE ${STATIC_LINK_FLAGS})

add_executable(thread_affinity thread_affinity.cpp)
target_link_libraries(thread_affinity PRIVATE elio)
target_link_options(thread_affinity PRIVATE ${STATIC_LINK_FLAGS})

add_executable(benchmark benchmark.cpp)
target_link_libraries(benchmark PRIVATE elio)
target_link_options(benchmark PRIVATE ${STATIC_LINK_FLAGS})

add_executable(quick_benchmark quick_benchmark.cpp)
target_link_libraries(quick_benchmark PRIVATE elio)
target_link_options(quick_benchmark PRIVATE ${STATIC_LINK_FLAGS})

add_executable(microbench microbench.cpp)
target_link_libraries(microbench PRIVATE elio)
target_link_options(microbench PRIVATE ${STATIC_LINK_FLAGS})

add_executable(io_benchmark io_benchmark.cpp)
target_link_libraries(io_benchmark PRIVATE elio)
target_link_options(io_benchmark PRIVATE ${STATIC_LINK_FLAGS})

add_executable(scalability_test scalability_test.cpp)
target_link_libraries(scalability_test PRIVATE elio)
target_link_options(scalability_test PRIVATE ${STATIC_LINK_FLAGS})

add_executable(bench_channel bench_channel.cpp)
target_link_libraries(bench_channel PRIVATE elio)
target_link_options(bench_channel PRIVATE ${STATIC_LINK_FLAGS})

# Signal handling example
add_executable(signal_handling signal_handling.cpp)
target_link_libraries(signal_handling PRIVATE elio)
target_link_options(signal_handling PRIVATE ${STATIC_LINK_FLAGS})

# Debug tools test
add_executable(debug_test debug_test.cpp)
target_link_libraries(debug_test PRIVATE elio)
target_link_options(debug_test PRIVATE ${STATIC_LINK_FLAGS})
target_compile_options(debug_test PRIVATE -g -O0)  # Full debug info
target_compile_definitions(debug_test PRIVATE ELIO_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# Network examples
add_executable(tcp_echo_server tcp_echo_server.cpp)
target_link_libraries(tcp_echo_server PRIVATE elio)
target_link_options(tcp_echo_server PRIVATE ${STATIC_LINK_FLAGS})

add_executable(tcp_echo_client tcp_echo_client.cpp)
target_link_libraries(tcp_echo_client PRIVATE elio)
target_link_options(tcp_echo_client PRIVATE ${STATIC_LINK_FLAGS})

add_executable(uds_echo_server uds_echo_server.cpp)
target_link_libraries(uds_echo_server PRIVATE elio)
target_link_options(uds_echo_server PRIVATE ${STATIC_LINK_FLAGS})

add_executable(uds_echo_client uds_echo_client.cpp)
target_link_libraries(uds_echo_client PRIVATE elio)
target_link_options(uds_echo_client PRIVATE ${STATIC_LINK_FLAGS})

# RPC examples
add_executable(rpc_server_example rpc_server_example.cpp)
target_link_libraries(rpc_server_example PRIVATE elio)
target_link_options(rpc_server_example PRIVATE ${STATIC_LINK_FLAGS})

add_executable(rpc_client_example rpc_client_example.cpp)
target_link_libraries(rpc_client_example PRIVATE elio)
target_link_options(rpc_client_example PRIVATE ${STATIC_LINK_FLAGS})

# Filesystem examples
add_executable(async_file_io async_file_io.cpp)
target_link_libraries(async_file_io PRIVATE elio)
target_link_options(async_file_io PRIVATE ${STATIC_LINK_FLAGS})

# HTTP examples (requires elio_http target)
if(TARGET elio_http)
    add_executable(http_server http_server.cpp)
    target_link_libraries(http_server PRIVATE elio_http)
    target_link_options(http_server PRIVATE ${STATIC_LINK_FLAGS})

    add_executable(http_client http_client.cpp)
    target_link_libraries(http_client PRIVATE elio_http)
    target_link_options(http_client PRIVATE ${STATIC_LINK_FLAGS})

    # HTTP/2 example (requires elio_http2 target)
    if(TARGET elio_http2)
        add_executable(http2_client http2_client.cpp)
        target_link_libraries(http2_client PRIVATE elio_http2)
        target_link_options(http2_client PRIVATE ${STATIC_LINK_FLAGS})
    endif()

    # WebSocket examples
    add_executable(websocket_server websocket_server.cpp)
    target_link_libraries(websocket_server PRIVATE elio_http)
    target_link_options(websocket_server PRIVATE ${STATIC_LINK_FLAGS})

    add_executable(websocket_client websocket_client.cpp)
    target_link_libraries(websocket_client PRIVATE elio_http)
    target_link_options(websocket_client PRIVATE ${STATIC_LINK_FLAGS})

    # Server-Sent Events examples
    add_executable(sse_server sse_server.cpp)
    target_link_libraries(sse_server PRIVATE elio_http)
    target_link_options(sse_server PRIVATE ${STATIC_LINK_FLAGS})

    add_executable(sse_client sse_client.cpp)
    target_link_libraries(sse_client PRIVATE elio_http)
    target_link_options(sse_client PRIVATE ${STATIC_LINK_FLAGS})
endif()

# RDMA mock-backend pingpong (requires elio_rdma).
if(TARGET elio_rdma)
    add_executable(rdma_pingpong_mock rdma_pingpong_mock.cpp)
    target_link_libraries(rdma_pingpong_mock PRIVATE elio_rdma)
    target_link_options(rdma_pingpong_mock PRIVATE ${STATIC_LINK_FLAGS})
endif()

# RDMA req/resp using endpoint + IMM (needs both ibverbs + CM helpers).
if(TARGET elio_rdma_ibverbs AND TARGET elio_rdma_cm)
    add_executable(rdma_req_resp_ibverbs rdma_req_resp_ibverbs.cpp)
    target_link_libraries(rdma_req_resp_ibverbs PRIVATE elio_rdma_ibverbs)
    target_link_options(rdma_req_resp_ibverbs PRIVATE ${STATIC_LINK_FLAGS})

    add_executable(rdma_perf rdma_perf.cpp)
    target_link_libraries(rdma_perf PRIVATE elio_rdma_ibverbs)
    target_link_options(rdma_perf PRIVATE ${STATIC_LINK_FLAGS})
endif()

if(TARGET elio_rdma_cuda)
    add_executable(rdma_gpu_bw rdma_gpu_bw.cpp)
    target_link_libraries(rdma_gpu_bw PRIVATE elio_rdma_cuda)
    target_link_options(rdma_gpu_bw PRIVATE ${STATIC_LINK_FLAGS})
endif()

# ---------------------------------------------------------------------------
# TCP loopback benchmarks: Elio vs libuv vs standalone Asio
# ---------------------------------------------------------------------------
if(ELIO_BUILD_TCP_BENCHMARKS)
    # Elio benchmark (always available when examples are built)
    add_executable(bench_tcp_elio bench_tcp_elio.cpp)
    target_link_libraries(bench_tcp_elio PRIVATE elio)
    target_link_options(bench_tcp_elio PRIVATE ${STATIC_LINK_FLAGS})

    # libuv benchmark (gated on system library)
    if(LIBUV_INCLUDE_DIR AND LIBUV_LIBRARY)
        add_executable(bench_tcp_libuv bench_tcp_libuv.cpp)
        target_include_directories(bench_tcp_libuv PRIVATE ${LIBUV_INCLUDE_DIR})
        target_link_libraries(bench_tcp_libuv PRIVATE ${LIBUV_LIBRARY} pthread dl)
        target_link_options(bench_tcp_libuv PRIVATE ${STATIC_LINK_FLAGS})
    endif()

    # Standalone Asio benchmark (gated on FetchContent target)
    if(TARGET asio)
        add_executable(bench_tcp_asio bench_tcp_asio.cpp)
        target_link_libraries(bench_tcp_asio PRIVATE asio pthread)
        target_link_options(bench_tcp_asio PRIVATE ${STATIC_LINK_FLAGS})
    endif()
endif()
