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

# Network examples
add_executable(tcp_echo_server tcp_echo_server.cpp)
target_link_libraries(tcp_echo_server PRIVATE elio)
target_link_options(tcp_echo_server PRIVATE ${STATIC_LINK_FLAGS})

add_executable(tcp_echo_client tcp_echo_client.cpp)
target_link_libraries(tcp_echo_client PRIVATE elio)
target_link_options(tcp_echo_client PRIVATE ${STATIC_LINK_FLAGS})

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
endif()
