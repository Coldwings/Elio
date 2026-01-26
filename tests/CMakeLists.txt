# Test sources
set(TEST_SOURCES
    test_main.cpp
    unit/test_logger.cpp
    unit/test_virtual_stack.cpp
    unit/test_task.cpp
    unit/test_awaitable_base.cpp
    unit/test_chase_lev_deque.cpp
    unit/test_scheduler.cpp
    unit/test_io.cpp
    unit/test_sync.cpp
    unit/test_timer.cpp
    integration/test_scheduler_integration.cpp
    integration/test_exception_propagation.cpp
    integration/test_parallel_tasks.cpp
    integration/test_dynamic_threads.cpp
)

# Add WebSocket and SSE tests if OpenSSL is available
if(OPENSSL_FOUND)
    list(APPEND TEST_SOURCES
        unit/test_websocket.cpp
        unit/test_sse.cpp
    )
endif()

# Standard test executable
add_executable(elio_tests ${TEST_SOURCES})
target_link_libraries(elio_tests PRIVATE elio Catch2::Catch2WithMain)
if(OPENSSL_FOUND)
    target_link_libraries(elio_tests PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
target_compile_definitions(elio_tests PRIVATE ELIO_DEBUG)

# ASAN-enabled test executable
add_executable(elio_tests_asan ${TEST_SOURCES})
target_link_libraries(elio_tests_asan PRIVATE elio Catch2::Catch2WithMain)
if(OPENSSL_FOUND)
    target_link_libraries(elio_tests_asan PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
target_compile_options(elio_tests_asan PRIVATE -fsanitize=address -g -fno-omit-frame-pointer)
target_link_options(elio_tests_asan PRIVATE -fsanitize=address)
target_compile_definitions(elio_tests_asan PRIVATE ELIO_DEBUG)

# TSAN-enabled test executable
add_executable(elio_tests_tsan ${TEST_SOURCES})
target_link_libraries(elio_tests_tsan PRIVATE elio Catch2::Catch2WithMain)
if(OPENSSL_FOUND)
    target_link_libraries(elio_tests_tsan PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
target_compile_options(elio_tests_tsan PRIVATE -fsanitize=thread -g -fno-omit-frame-pointer -Wno-tsan)
target_link_options(elio_tests_tsan PRIVATE -fsanitize=thread)
target_compile_definitions(elio_tests_tsan PRIVATE ELIO_DEBUG)

# Register tests with CTest
include(Catch)
catch_discover_tests(elio_tests)
catch_discover_tests(elio_tests_asan)
catch_discover_tests(elio_tests_tsan)

# Add custom targets for convenience
add_custom_target(test_all
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS elio_tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_custom_target(test_asan
    COMMAND ${CMAKE_CTEST_COMMAND} -R elio_tests_asan --output-on-failure
    DEPENDS elio_tests_asan
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_custom_target(test_tsan
    COMMAND ${CMAKE_CTEST_COMMAND} -R elio_tests_tsan --output-on-failure
    DEPENDS elio_tests_tsan
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
