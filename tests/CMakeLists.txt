# Test sources
if(ELIO_ENABLE_DEVELOPER_WARNINGS)
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(ELIO_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

set(TEST_SOURCES
    test_main.cpp
    test_watchdog.cpp
    unit/test_logger.cpp
    unit/test_virtual_stack.cpp
    unit/test_vthread_stack.cpp
    unit/test_task.cpp
    unit/test_awaitable_base.cpp
    unit/test_chase_lev_deque.cpp
    unit/test_scheduler.cpp
    unit/test_scheduler_shutdown.cpp
    unit/test_blocking_pool.cpp
    unit/test_spawn_blocking_uaf.cpp
    unit/test_affinity.cpp
    unit/test_io.cpp
    unit/test_batch_io.cpp
    unit/test_sync.cpp
    unit/test_intrusive_list.cpp
    unit/test_sync_cancellation.cpp
    unit/test_channel_debug.cpp
    unit/test_channel_close.cpp
    unit/test_channel_success.cpp
    unit/test_close_wake.cpp
    unit/test_simple_channel.cpp
    unit/test_cancel_token.cpp
    unit/test_timer.cpp
    unit/test_hash.cpp
    unit/test_rpc.cpp
    unit/test_signalfd.cpp
    unit/test_autoscaler.cpp
    unit/test_generator.cpp
    unit/test_object_cache.cpp
    unit/test_combinators.cpp
    integration/test_scheduler_integration.cpp
    integration/test_exception_propagation.cpp
    integration/test_parallel_tasks.cpp
    integration/test_dynamic_threads.cpp
)

# Add HTTP tests (no TLS dependency for parser tests)
list(APPEND TEST_SOURCES
    unit/test_http_parser.cpp
)
if(TARGET elio_http)
    list(APPEND TEST_SOURCES
        unit/test_http_server.cpp
    )
endif()

# HTTP client integration tests need TLS because the client surface owns TLS
# context setup for HTTPS connections.
if(TARGET elio_tls)
    list(APPEND TEST_SOURCES
        unit/test_http_client.cpp
    )
endif()

# Add HTTP/2 tests if nghttp2 is available
if(TARGET nghttp2_static)
    list(APPEND TEST_SOURCES
        unit/test_http2.cpp
    )
endif()

# Add WebSocket and SSE tests if TLS is available
if(TARGET elio_tls)
    list(APPEND TEST_SOURCES
        unit/test_websocket.cpp
        unit/test_sse.cpp
    )
endif()

# Keep copyable public-header examples checked against the enabled HTTP/TLS API.
# This object target compiles once and intentionally is not linked or executed.
if(TARGET elio_http AND TARGET elio_tls)
    add_library(elio_public_header_examples OBJECT
        compile/public_header_examples.cpp
    )
    target_link_libraries(elio_public_header_examples PRIVATE
        elio
        elio_http
        elio_tls
    )
endif()

# RDMA abstraction tests (only when ELIO_ENABLE_RDMA=ON).
# Header-only; no extra libraries required for the dispatcher / op_state
# slice — the mock backend (S3+) and any real ibverbs backend are
# user-side, never built into elio_tests.
if(TARGET elio_rdma)
    list(APPEND TEST_SOURCES
        unit/test_rdma_dispatcher.cpp
        unit/test_rdma_polymorphic_backend.cpp
        unit/test_rdma_send_recv.cpp
        unit/test_rdma_one_sided.cpp
        unit/test_rdma_sge.cpp
        unit/test_rdma_inline.cpp
        unit/test_rdma_srq.cpp
        unit/test_rdma_memory_region.cpp
        unit/test_rdma_cq_pump.cpp
        unit/test_rdma_imm.cpp
        unit/test_rdma_atomic.cpp
    )
endif()

# elio_rdma_cm tests (S8). Requires librdmacm at compile + link.
if(TARGET elio_rdma_cm)
    list(APPEND TEST_SOURCES
        unit/test_rdma_cm.cpp
    )
endif()

# elio_rdma_ibverbs backend tests. Requires libibverbs at compile + link,
# but no RDMA device at runtime.
if(TARGET elio_rdma_ibverbs)
    list(APPEND TEST_SOURCES
        unit/test_rdma_ibverbs_backend.cpp
    )
endif()

# elio_rdma_ibverbs endpoint smoke tests (S13). Requires libibverbs
# and librdmacm at compile + link.
if(TARGET elio_rdma_ibverbs AND TARGET elio_rdma_cm)
    list(APPEND TEST_SOURCES
        unit/test_rdma_ibverbs_endpoint.cpp
    )
endif()

# Standard test executable
add_executable(elio_tests ${TEST_SOURCES})
target_link_libraries(elio_tests PRIVATE elio Catch2::Catch2WithMain)
if(TARGET elio_http)
    target_link_libraries(elio_tests PRIVATE elio_http)
endif()
if(TARGET elio_tls)
    target_link_libraries(elio_tests PRIVATE elio_tls)
endif()
if(TARGET nghttp2_static)
    target_link_libraries(elio_tests PRIVATE nghttp2_static)
endif()
if(TARGET elio_rdma_cm)
    target_link_libraries(elio_tests PRIVATE elio_rdma_cm)
endif()
if(TARGET elio_rdma_ibverbs)
    target_link_libraries(elio_tests PRIVATE elio_rdma_ibverbs)
endif()
target_compile_definitions(elio_tests PRIVATE ELIO_DEBUG)

# ASAN-enabled test executable
add_executable(elio_tests_asan ${TEST_SOURCES})
target_link_libraries(elio_tests_asan PRIVATE elio Catch2::Catch2WithMain)
if(TARGET elio_http)
    target_link_libraries(elio_tests_asan PRIVATE elio_http)
endif()
if(TARGET elio_tls)
    target_link_libraries(elio_tests_asan PRIVATE elio_tls)
endif()
if(TARGET nghttp2_static)
    target_link_libraries(elio_tests_asan PRIVATE nghttp2_static)
endif()
if(TARGET elio_rdma_cm)
    target_link_libraries(elio_tests_asan PRIVATE elio_rdma_cm)
endif()
if(TARGET elio_rdma_ibverbs)
    target_link_libraries(elio_tests_asan PRIVATE elio_rdma_ibverbs)
endif()
target_compile_options(elio_tests_asan PRIVATE -fsanitize=address -g -fno-omit-frame-pointer)
target_link_options(elio_tests_asan PRIVATE -fsanitize=address)
target_compile_definitions(elio_tests_asan PRIVATE ELIO_DEBUG)

# TSAN-enabled test executable
add_executable(elio_tests_tsan ${TEST_SOURCES})
target_link_libraries(elio_tests_tsan PRIVATE elio Catch2::Catch2WithMain)
if(TARGET elio_http)
    target_link_libraries(elio_tests_tsan PRIVATE elio_http)
endif()
if(TARGET elio_tls)
    target_link_libraries(elio_tests_tsan PRIVATE elio_tls)
endif()
if(TARGET nghttp2_static)
    target_link_libraries(elio_tests_tsan PRIVATE nghttp2_static)
endif()
if(TARGET elio_rdma_cm)
    target_link_libraries(elio_tests_tsan PRIVATE elio_rdma_cm)
endif()
if(TARGET elio_rdma_ibverbs)
    target_link_libraries(elio_tests_tsan PRIVATE elio_rdma_ibverbs)
endif()
target_compile_options(elio_tests_tsan PRIVATE -fsanitize=thread -g -fno-omit-frame-pointer -Wno-tsan)
target_link_options(elio_tests_tsan PRIVATE -fsanitize=thread)
target_compile_definitions(elio_tests_tsan PRIVATE ELIO_DEBUG)

# Stress test executable for shared_mutex
add_executable(stress_test_shared_mutex stress_test_shared_mutex.cpp)
target_link_libraries(stress_test_shared_mutex PRIVATE elio)
if(TARGET elio_tls)
    target_link_libraries(stress_test_shared_mutex PRIVATE elio_tls)
endif()
if(TARGET nghttp2_static)
    target_link_libraries(stress_test_shared_mutex PRIVATE nghttp2_static)
endif()
target_compile_definitions(stress_test_shared_mutex PRIVATE ELIO_DEBUG)

# Register tests with CTest
include(Catch)
catch_discover_tests(elio_tests)
catch_discover_tests(elio_tests_asan
    TEST_PREFIX "elio_tests_asan."
)
catch_discover_tests(elio_tests_tsan
    TEST_PREFIX "elio_tests_tsan."
)

add_test(NAME package_rdma_cuda_export_metadata
    COMMAND ${CMAKE_COMMAND}
        -DELIO_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DELIO_BINARY_DIR=${CMAKE_BINARY_DIR}/tests/cmake/package_rdma_cuda_export
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_rdma_cuda_package_export.cmake
)

add_test(NAME package_liburing_export_metadata
    COMMAND ${CMAKE_COMMAND}
        -DELIO_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DELIO_BINARY_DIR=${CMAKE_BINARY_DIR}/tests/cmake/package_liburing_export
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_liburing_package_export.cmake
)

add_test(NAME package_rdma_dependency_export_metadata
    COMMAND ${CMAKE_COMMAND}
        -DELIO_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DELIO_BINARY_DIR=${CMAKE_BINARY_DIR}/tests/cmake/package_rdma_dependency_export
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_rdma_dependency_package_export.cmake
)

add_test(NAME package_elio_compile_feature_export
    COMMAND ${CMAKE_COMMAND}
        -DELIO_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DELIO_BINARY_DIR=${CMAKE_BINARY_DIR}/tests/cmake/package_elio_compile_feature_export
        -DELIO_PARENT_BINARY_DIR=${CMAKE_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_elio_compile_feature_export.cmake
)

if(TARGET nghttp2_static)
    add_test(NAME http2_fetchcontent_cache_scope
        COMMAND ${CMAKE_COMMAND}
            -DELIO_SOURCE_DIR=${PROJECT_SOURCE_DIR}
            -DELIO_BINARY_DIR=${CMAKE_BINARY_DIR}/tests/cmake/http2_fetchcontent_cache_scope
            -DELIO_PARENT_BINARY_DIR=${CMAKE_BINARY_DIR}
            -DELIO_PARENT_FMT_SOURCE_DIR=${fmt_SOURCE_DIR}
            -DELIO_PARENT_NGHTTP2_SOURCE_DIR=${nghttp2_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_http2_fetchcontent_cache_scope.cmake
    )
endif()

add_test(NAME package_version_compatibility
    COMMAND ${CMAKE_COMMAND}
        -DELIO_VERSION_FILE=${CMAKE_BINARY_DIR}/ElioConfigVersion.cmake
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_elio_version_compatibility.cmake
)

add_executable(elio_rdma_cuda_lifetime_tests
    unit/test_rdma_cuda_gpu_memory_region.cpp
)
target_include_directories(elio_rdma_cuda_lifetime_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs/rdma_cuda
)
target_link_libraries(elio_rdma_cuda_lifetime_tests PRIVATE
    elio Catch2::Catch2WithMain)
target_compile_definitions(elio_rdma_cuda_lifetime_tests PRIVATE ELIO_DEBUG)
catch_discover_tests(elio_rdma_cuda_lifetime_tests)
set(ELIO_ADDITIONAL_TEST_TARGETS elio_rdma_cuda_lifetime_tests)

if(NOT TARGET elio_rdma_ibverbs)
    add_executable(elio_rdma_ibverbs_backend_stub_tests
        unit/test_rdma_ibverbs_backend.cpp
    )
    target_include_directories(elio_rdma_ibverbs_backend_stub_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs/rdma_cuda
    )
    target_link_libraries(elio_rdma_ibverbs_backend_stub_tests PRIVATE
        elio Catch2::Catch2WithMain)
    target_compile_definitions(elio_rdma_ibverbs_backend_stub_tests PRIVATE
        ELIO_DEBUG)
    catch_discover_tests(elio_rdma_ibverbs_backend_stub_tests)
    list(APPEND ELIO_ADDITIONAL_TEST_TARGETS
        elio_rdma_ibverbs_backend_stub_tests)
endif()

# Optional libibverbs-backed RDMA integration test (S10). The binary
# is separate so linking libibverbs is opt-in and devices without
# rxe simply skip the test rather than failing the build. Discovered
# here so that `include(Catch)` from earlier in this file is in
# scope.
if(ELIO_ENABLE_RDMA_IBVERBS_TESTS AND TARGET elio_rdma_ibverbs)
    add_executable(elio_rdma_integration_tests
        test_main.cpp
        integration_rdma/test_rdma_rxe_loopback.cpp
    )
    target_link_libraries(elio_rdma_integration_tests PRIVATE
        elio_rdma_ibverbs Catch2::Catch2WithMain)
    target_compile_definitions(elio_rdma_integration_tests PRIVATE ELIO_DEBUG)
    catch_discover_tests(elio_rdma_integration_tests)
elseif(ELIO_ENABLE_RDMA_IBVERBS_TESTS)
    message(FATAL_ERROR
        "ELIO_ENABLE_RDMA_IBVERBS_TESTS=ON requires ELIO_ENABLE_RDMA_IBVERBS=ON")
endif()

# Add custom targets for convenience
add_custom_target(test_all
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS elio_tests ${ELIO_ADDITIONAL_TEST_TARGETS}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_custom_target(test_asan
    COMMAND ${CMAKE_CTEST_COMMAND} -R "^elio_tests_asan\\." --output-on-failure
    DEPENDS elio_tests_asan
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_custom_target(test_tsan
    COMMAND ${CMAKE_CTEST_COMMAND} -R "^elio_tests_tsan\\." --output-on-failure
    DEPENDS elio_tests_tsan
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
