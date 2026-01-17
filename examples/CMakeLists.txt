# Example programs
add_executable(hello_world hello_world.cpp)
target_link_libraries(hello_world PRIVATE elio)

add_executable(chained_coroutines chained_coroutines.cpp)
target_link_libraries(chained_coroutines PRIVATE elio)

add_executable(exception_handling exception_handling.cpp)
target_link_libraries(exception_handling PRIVATE elio)

add_executable(parallel_tasks parallel_tasks.cpp)
target_link_libraries(parallel_tasks PRIVATE elio)

add_executable(dynamic_threads dynamic_threads.cpp)
target_link_libraries(dynamic_threads PRIVATE elio)

add_executable(benchmark benchmark.cpp)
target_link_libraries(benchmark PRIVATE elio)

# Network examples
add_executable(tcp_echo_server tcp_echo_server.cpp)
target_link_libraries(tcp_echo_server PRIVATE elio)

add_executable(tcp_echo_client tcp_echo_client.cpp)
target_link_libraries(tcp_echo_client PRIVATE elio)

# Filesystem examples
add_executable(async_file_io async_file_io.cpp)
target_link_libraries(async_file_io PRIVATE elio)

# HTTP examples (requires OpenSSL)
if(OPENSSL_FOUND)
    add_executable(http_server http_server.cpp)
    target_link_libraries(http_server PRIVATE elio_http)

    add_executable(http_client http_client.cpp)
    target_link_libraries(http_client PRIVATE elio_http)
endif()
