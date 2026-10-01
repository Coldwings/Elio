foreach(_required IN ITEMS ELIO_SOURCE_DIR ELIO_BINARY_DIR
        ELIO_FMT_SOURCE_DIR ELIO_CATCH2_SOURCE_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

# Configure only: inspect real target edges without compiling sanitizer suites
# or requiring RDMA hardware. Reuse the parent dependency sources, not network.
file(MAKE_DIRECTORY "${ELIO_BINARY_DIR}/source")
file(WRITE "${ELIO_BINARY_DIR}/source/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.20)
project(elio_test_all_dependency_probe LANGUAGES CXX)
add_subdirectory("${ELIO_SOURCE_DIR}" elio)

set(_expected elio_tests elio_file_transfer_header elio_byte_stream_header
    elio_resolve_wait_header elio_fork_boundary_tests
    elio_rdma_cuda_lifetime_tests elio_test_watchdog_probe_normal)
if(NOT ELIO_ENABLE_RDMA_IBVERBS)
    list(APPEND _expected elio_rdma_ibverbs_backend_stub_tests)
endif()
if(ELIO_BUILD_EXAMPLES)
    list(APPEND _expected first_coroutine join_destroyed_async positional_file_io)
endif()
if(ELIO_BUILD_SANITIZER_TESTS)
    list(APPEND _expected elio_tests_asan elio_tests_tsan elio_fork_boundary_tests_asan
        elio_test_watchdog_probe_asan elio_test_watchdog_probe_tsan)
endif()
if(ELIO_BUILD_HTTP_INTEROP_TESTS)
    list(APPEND _expected elio_http_streaming_peer elio_http_connect_peer
        elio_http_streaming_cost_probe elio_http_streaming_transport_tests)
endif()
if(TARGET elio_http)
    list(APPEND _expected elio_websocket_client_header elio_websocket_server_header
        elio_websocket_client_server_header elio_websocket_server_client_header)
endif()
if(ELIO_BUILD_HTTP_METRICS)
    list(APPEND _expected elio_http_streaming_metrics_server)
endif()
if(ELIO_ENABLE_RDMA_IBVERBS_TESTS)
    list(APPEND _expected elio_rdma_integration_tests)
endif()
get_target_property(_actual test_all MANUALLY_ADDED_DEPENDENCIES)
foreach(_target IN LISTS _expected)
    if(NOT TARGET ${_target} OR NOT _target IN_LIST _actual)
        message(FATAL_ERROR "test_all is missing registered executable ${_target}")
    endif()
endforeach()
foreach(_target IN LISTS _actual)
    if(NOT TARGET ${_target})
        message(FATAL_ERROR "test_all depends on unavailable target ${_target}")
    endif()
    if(_target MATCHES "^(stress_test_|bench|quick_benchmark|microbench)")
        message(FATAL_ERROR "test_all unexpectedly requires unregistered ${_target}")
    endif()
endforeach()
]=])

set(_variants tests_only examples_sanitizers)
if(ELIO_CHECK_HTTP)
    list(APPEND _variants http_fixtures)
endif()
if(ELIO_CHECK_RDMA)
    list(APPEND _variants rdma_integration)
endif()
set(_parent_args)
if(ELIO_PARENT_CMAKE_GENERATOR)
    list(APPEND _parent_args -G "${ELIO_PARENT_CMAKE_GENERATOR}")
endif()
foreach(_name IN ITEMS CMAKE_MAKE_PROGRAM CMAKE_CXX_COMPILER
        CMAKE_TOOLCHAIN_FILE CMAKE_SYSROOT CMAKE_FIND_ROOT_PATH CMAKE_PREFIX_PATH
        OPENSSL_INCLUDE_DIR OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY
        _IBVERBS_INC_PATH _IBVERBS_LIB_PATH Python3_EXECUTABLE
        CMAKE_DISABLE_FIND_PACKAGE_Python3)
    if(DEFINED ELIO_PARENT_${_name} AND
       NOT "${ELIO_PARENT_${_name}}" STREQUAL "" AND
       NOT "${ELIO_PARENT_${_name}}" MATCHES "-NOTFOUND$")
        string(REPLACE ";" "\\;" _value "${ELIO_PARENT_${_name}}")
        list(APPEND _parent_args "-D${_name}=${_value}")
    endif()
endforeach()
foreach(_variant IN LISTS _variants)
    set(_examples OFF)
    set(_sanitizers OFF)
    set(_http OFF)
    set(_rdma OFF)
    if(_variant STREQUAL "examples_sanitizers")
        set(_examples ON)
        set(_sanitizers ON)
    elseif(_variant STREQUAL "http_fixtures")
        set(_http ON)
    elseif(_variant STREQUAL "rdma_integration")
        set(_rdma ON)
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}"
        -S "${ELIO_BINARY_DIR}/source" -B "${ELIO_BINARY_DIR}/${_variant}"
        ${_parent_args}
        -DELIO_SOURCE_DIR=${ELIO_SOURCE_DIR}
        -DFETCHCONTENT_SOURCE_DIR_FMT=${ELIO_FMT_SOURCE_DIR}
        -DFETCHCONTENT_SOURCE_DIR_CATCH2=${ELIO_CATCH2_SOURCE_DIR}
        -DELIO_BUILD_TESTS=ON -DELIO_BUILD_EXAMPLES=${_examples}
        -DELIO_BUILD_SANITIZER_TESTS=${_sanitizers}
        -DELIO_ENABLE_TLS=${_http} -DELIO_ENABLE_HTTP=${_http}
        -DELIO_ENABLE_HTTP2=OFF
        -DELIO_BUILD_HTTP_INTEROP_TESTS=${_http} -DELIO_BUILD_HTTP_METRICS=${_http}
        -DELIO_ENABLE_RDMA=${_rdma} -DELIO_ENABLE_RDMA_IBVERBS=${_rdma}
        -DELIO_ENABLE_RDMA_IBVERBS_TESTS=${_rdma}
        -DELIO_ENABLE_RDMA_CM=OFF -DELIO_ENABLE_RDMA_CUDA=OFF
        -DELIO_BUILD_TCP_BENCHMARKS=OFF
        RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "${_variant} target graph failed:\n${_output}\n${_error}")
    endif()
endforeach()
