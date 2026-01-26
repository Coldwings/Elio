# Example programs

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

add_executable(benchmark benchmark.cpp)
target_link_libraries(benchmark PRIVATE elio)
target_link_options(benchmark PRIVATE ${STATIC_LINK_FLAGS})

# Debug tools test
add_executable(debug_test debug_test.cpp)
target_link_libraries(debug_test PRIVATE elio)
target_link_options(debug_test PRIVATE ${STATIC_LINK_FLAGS})
target_compile_options(debug_test PRIVATE -g -O0)  # Full debug info

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

# HTTP examples (requires OpenSSL)
if(OPENSSL_FOUND)
    add_executable(http_server http_server.cpp)
    target_link_libraries(http_server PRIVATE elio_http)
    target_link_options(http_server PRIVATE ${STATIC_LINK_FLAGS})

    add_executable(http_client http_client.cpp)
    target_link_libraries(http_client PRIVATE elio_http)
    target_link_options(http_client PRIVATE ${STATIC_LINK_FLAGS})

    # HTTP/2 example (requires nghttp2)
    add_executable(http2_client http2_client.cpp)
    target_link_libraries(http2_client PRIVATE elio_http2)
    target_link_options(http2_client PRIVATE ${STATIC_LINK_FLAGS})

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
