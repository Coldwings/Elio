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
    unit/test_bench_tcp_common.cpp
    unit/test_logger.cpp
    unit/test_virtual_stack.cpp
    unit/test_frame_allocation.cpp
    unit/test_task_execution_context.cpp
    unit/test_task.cpp
    unit/test_task_group.cpp
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
    unit/test_cancellation_context.cpp
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

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$" AND
   CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    list(APPEND TEST_SOURCES
        unit/crc32c_capability_baseline.cpp
        unit/crc32c_capability_sse42.cpp
    )
    set_source_files_properties(unit/crc32c_capability_baseline.cpp
        PROPERTIES COMPILE_OPTIONS -mno-sse4.2)
    set_source_files_properties(unit/crc32c_capability_sse42.cpp
        PROPERTIES COMPILE_OPTIONS -msse4.2)
endif()

# Add HTTP tests (no TLS dependency for parser tests)
list(APPEND TEST_SOURCES
    unit/test_http_parser.cpp
    unit/test_http_response_decoder.cpp
    unit/test_http_response_reader.cpp
    unit/test_http_response_plan.cpp
    unit/test_http_body_writer.cpp
    unit/test_http_response_sender.cpp
    unit/test_sse_writer.cpp
)
if(TARGET elio_http)
    list(APPEND TEST_SOURCES
        unit/test_http_server.cpp
        unit/test_http_streaming_server.cpp
        unit/test_http_streaming_transport.cpp
    )
endif()

# HTTP client integration tests need TLS because the client surface owns TLS
# context setup for HTTPS connections.
if(TARGET elio_tls)
    list(APPEND TEST_SOURCES
        unit/test_tls_retry.cpp
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
        compile/http_common_standalone.cpp
        compile/http_streaming_migration.cpp
        compile/public_header_examples.cpp
    )
    target_link_libraries(elio_public_header_examples PRIVATE
        elio
        elio_http
        elio_tls
    )
endif()

option(ELIO_BUILD_HTTP_INTEROP_TESTS "Build HTTP streaming interoperability fixture (requires h11==0.16.0)" OFF)
if(ELIO_BUILD_HTTP_INTEROP_TESTS)
    if(NOT TARGET elio_http OR NOT TARGET elio_tls)
        message(FATAL_ERROR "HTTP interoperability tests require HTTP and TLS targets")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(elio_http_streaming_peer integration/http_streaming_peer_server.cpp)
    target_link_libraries(elio_http_streaming_peer PRIVATE elio_http elio_tls)
    add_executable(elio_http_streaming_cost_probe integration/http_streaming_cost_probe.cpp)
    target_link_libraries(elio_http_streaming_cost_probe PRIVATE elio)
    add_test(NAME http_streaming_cost_probe COMMAND elio_http_streaming_cost_probe)
    add_executable(elio_http_streaming_transport_tests unit/test_http_streaming_transport.cpp)
    target_link_libraries(elio_http_streaming_transport_tests PRIVATE elio_http elio_tls Catch2::Catch2WithMain)
    target_compile_definitions(elio_http_streaming_transport_tests PRIVATE ELIO_DEBUG ELIO_RUNTIME_TEST_HOOKS)
    add_test(NAME http_streaming_interop
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/http_streaming_peer.py
            --server $<TARGET_FILE:elio_http_streaming_peer>)
    set_tests_properties(http_streaming_interop PROPERTIES TIMEOUT 90)
endif()

option(ELIO_BUILD_HTTP_METRICS "Build real HTTP TCP/TLS diagnostic fixture (requires h11==0.16.0)" OFF)
if(ELIO_BUILD_HTTP_METRICS)
    if(NOT TARGET elio_http OR NOT TARGET elio_tls)
        message(FATAL_ERROR "HTTP metrics require HTTP and TLS targets")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(elio_http_streaming_metrics_server integration/http_streaming_metrics_server.cpp)
    target_link_libraries(elio_http_streaming_metrics_server PRIVATE elio_http elio_tls)
    execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        OUTPUT_VARIABLE ELIO_HTTP_METRICS_HEAD OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE ELIO_HTTP_METRICS_HEAD_RESULT ERROR_QUIET)
    if(NOT ELIO_HTTP_METRICS_HEAD_RESULT EQUAL 0)
        set(ELIO_HTTP_METRICS_HEAD "unknown")
    endif()
    execute_process(COMMAND git status --porcelain WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        OUTPUT_VARIABLE ELIO_HTTP_METRICS_STATUS OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE ELIO_HTTP_METRICS_STATUS_RESULT ERROR_QUIET)
    if(ELIO_HTTP_METRICS_STATUS_RESULT EQUAL 0 AND ELIO_HTTP_METRICS_STATUS STREQUAL "")
        set(ELIO_HTTP_METRICS_DIRTY "false")
    else()
        set(ELIO_HTTP_METRICS_DIRTY "true")
    endif()
    target_compile_definitions(elio_http_streaming_metrics_server PRIVATE
        ELIO_HTTP_METRICS_BUILD_TYPE="$<CONFIG>"
        ELIO_HTTP_METRICS_BUILD_HEAD="${ELIO_HTTP_METRICS_HEAD}"
        ELIO_HTTP_METRICS_BUILD_DIRTY=${ELIO_HTTP_METRICS_DIRTY})
    add_test(NAME http_streaming_metrics_report_tests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_http_streaming_metrics.py)
    set_tests_properties(http_streaming_metrics_report_tests PROPERTIES TIMEOUT 30)
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

# Standalone process-boundary test. Keeping this outside Catch2 guarantees that
# its first fork occurs before any Elio runtime has existed in the process.
set(ELIO_ADDITIONAL_TEST_TARGETS)
add_executable(elio_fork_boundary_tests fork_boundary_tests.cpp)
target_link_libraries(elio_fork_boundary_tests PRIVATE elio)
target_compile_definitions(elio_fork_boundary_tests PRIVATE ELIO_DEBUG)
list(APPEND ELIO_ADDITIONAL_TEST_TARGETS elio_fork_boundary_tests)

if(ELIO_BUILD_SANITIZER_TESTS)
    add_executable(elio_fork_boundary_tests_asan fork_boundary_tests.cpp)
    target_link_libraries(elio_fork_boundary_tests_asan PRIVATE elio)
    target_compile_options(elio_fork_boundary_tests_asan PRIVATE
        -fsanitize=address -g -fno-omit-frame-pointer)
    target_link_options(elio_fork_boundary_tests_asan PRIVATE -fsanitize=address)
    target_compile_definitions(elio_fork_boundary_tests_asan PRIVATE ELIO_DEBUG)
    list(APPEND ELIO_ADDITIONAL_TEST_TARGETS elio_fork_boundary_tests_asan)
endif()
# Do not add a TSAN variant: the sanitizer runtime is already multithreaded,
# so its child cannot exercise the supported single-threaded pre-runtime fork.
# Active-runtime child behavior is limited to exec/_exit and covered without
# pretending TSAN can validate inherited-runtime continuation.

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
target_compile_definitions(elio_tests PRIVATE ELIO_DEBUG ELIO_RUNTIME_TEST_HOOKS)

if(ELIO_BUILD_SANITIZER_TESTS)
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
    target_compile_definitions(elio_tests_asan PRIVATE ELIO_DEBUG ELIO_RUNTIME_TEST_HOOKS)

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
    target_compile_definitions(elio_tests_tsan PRIVATE ELIO_DEBUG ELIO_RUNTIME_TEST_HOOKS)
endif()

add_test(NAME elio_fork_boundary COMMAND elio_fork_boundary_tests)
set_tests_properties(elio_fork_boundary PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
if(ELIO_BUILD_SANITIZER_TESTS)
    add_test(NAME elio_fork_boundary_asan COMMAND elio_fork_boundary_tests_asan)
    set_tests_properties(elio_fork_boundary_asan PROPERTIES
        TIMEOUT 60
        RUN_SERIAL TRUE)
endif()

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
if(ELIO_BUILD_SANITIZER_TESTS)
    catch_discover_tests(elio_tests_asan
        TEST_PREFIX "elio_tests_asan."
    )
    catch_discover_tests(elio_tests_tsan
        TEST_PREFIX "elio_tests_tsan."
    )
endif()

add_test(NAME package_rdma_cuda_export_metadata
    COMMAND ${CMAKE_COMMAND}
        -DELIO_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DELIO_BINARY_DIR=${CMAKE_BINARY_DIR}/tests/cmake/package_rdma_cuda_export
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_rdma_cuda_package_export.cmake
)

add_test(NAME rdma_cuda_example_api_metadata
    COMMAND ${CMAKE_COMMAND}
        -DELIO_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_rdma_cuda_example_api.cmake
)

add_test(NAME package_liburing_export_metadata
    COMMAND ${CMAKE_COMMAND}
        -DELIO_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DELIO_BINARY_DIR=${CMAKE_BINARY_DIR}/tests/cmake/package_liburing_export
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_liburing_package_export.cmake
)

add_test(NAME package_threads_export_metadata
    COMMAND ${CMAKE_COMMAND}
        -DELIO_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DELIO_BINARY_DIR=${CMAKE_BINARY_DIR}/tests/cmake/package_threads_export
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_threads_package_export.cmake
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

add_test(NAME http2_option_requires_http
    COMMAND ${CMAKE_COMMAND}
        -DELIO_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DELIO_BINARY_DIR=${CMAKE_BINARY_DIR}/tests/cmake/http2_option_requires_http
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_http2_option_dependencies.cmake
)

add_test(NAME non_linux_platform_rejected
    COMMAND ${CMAKE_COMMAND}
        -DELIO_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DELIO_BINARY_DIR=${CMAKE_BINARY_DIR}/tests/cmake/non_linux_platform
        -DELIO_FMT_SOURCE_DIR=${fmt_SOURCE_DIR}
        -DELIO_CMAKE_GENERATOR=${CMAKE_GENERATOR}
        -DELIO_CMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
        -DELIO_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_linux_platform.cmake
)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME debugger_script_helpers
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_debugger_scripts.py
    )
    add_test(NAME capture_test_evidence_helper
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_capture_test_evidence.py
    )
    add_test(NAME tcp_benchmark_conformance_summary
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_tcp_benchmark_conformance_summary.py
    )
endif()

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
list(APPEND ELIO_ADDITIONAL_TEST_TARGETS elio_rdma_cuda_lifetime_tests)

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
        ELIO_DEBUG
        ELIO_HAS_RDMA_IBVERBS=1)
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

if(ELIO_BUILD_SANITIZER_TESTS)
    add_custom_target(test_asan
        COMMAND ${CMAKE_CTEST_COMMAND}
            -R "^(elio_tests_asan\\..*|elio_fork_boundary_asan)$"
            --output-on-failure
        DEPENDS elio_tests_asan elio_fork_boundary_tests_asan
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )

    add_custom_target(test_tsan
        COMMAND ${CMAKE_CTEST_COMMAND} -R "^elio_tests_tsan\\." --output-on-failure
        DEPENDS elio_tests_tsan
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()
