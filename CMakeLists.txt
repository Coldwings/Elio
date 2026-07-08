cmake_minimum_required(VERSION 3.20)
project(Elio VERSION 0.5.2 LANGUAGES CXX)

include(CMakePackageConfigHelpers)

# Determine if Elio is the top-level project or included via add_subdirectory/FetchContent
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(ELIO_IS_TOP_LEVEL TRUE)
else()
    set(ELIO_IS_TOP_LEVEL FALSE)
endif()

# Options - defaults are ON for top-level builds, OFF when used as a dependency
option(ELIO_BUILD_TESTS "Build Elio tests" ${ELIO_IS_TOP_LEVEL})
option(ELIO_BUILD_EXAMPLES "Build Elio examples" ${ELIO_IS_TOP_LEVEL})
option(ELIO_ENABLE_TLS "Enable TLS support (requires OpenSSL)" ${ELIO_IS_TOP_LEVEL})
option(ELIO_ENABLE_HTTP "Enable HTTP client/server support (requires TLS)" ${ELIO_IS_TOP_LEVEL})
option(ELIO_ENABLE_HTTP2 "Enable HTTP/2 support (requires nghttp2)" ${ELIO_IS_TOP_LEVEL})
option(ELIO_ENABLE_RDMA "Enable RDMA-Core abstraction layer (header-only; user supplies ibverbs backend)" OFF)
option(ELIO_ENABLE_RDMA_CM "Enable optional librdmacm-backed CM helper (requires librdmacm)" OFF)
option(ELIO_ENABLE_RDMA_IBVERBS "Enable optional libibverbs backend + cq_drain helper + endpoint wrapper (requires libibverbs)" OFF)
option(ELIO_ENABLE_RDMA_IBVERBS_TESTS "Build the libibverbs-backed RDMA integration test (requires libibverbs + rxe at runtime)" OFF)
option(ELIO_ENABLE_RDMA_CUDA "Enable CUDA GPUDirect RDMA helpers (requires CUDA Toolkit + elio_rdma_ibverbs)" OFF)
option(ELIO_ENABLE_DEVELOPER_WARNINGS "Enable strict warning flags for Elio tests/examples" ${ELIO_IS_TOP_LEVEL})
option(ELIO_WARNINGS_AS_ERRORS "Treat warnings as errors for Elio tests/examples" ${ELIO_IS_TOP_LEVEL})
option(ELIO_ENABLE_DEBUG_METADATA "Enable coroutine debug metadata" ON)

# Platform check - Linux only
if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "Elio currently only supports Linux")
endif()

# C++20 requirement
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate compile_commands.json for clangd/LSP
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Header-only library interface
add_library(elio INTERFACE)
target_include_directories(elio INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Debug metadata option
if(ELIO_ENABLE_DEBUG_METADATA)
    target_compile_definitions(elio INTERFACE ELIO_ENABLE_DEBUG_METADATA=1)
else()
    target_compile_definitions(elio INTERFACE ELIO_ENABLE_DEBUG_METADATA=0)
endif()

# Dependencies via FetchContent
include(FetchContent)

# fmtlib for logging
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(fmt)

# Catch2 for testing (only if tests are enabled)
if(ELIO_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)
endif()

# Check for liburing (optional, for io_uring backend)
find_library(URING_LIBRARY uring)
find_path(URING_INCLUDE_DIR liburing.h)

if(URING_LIBRARY AND URING_INCLUDE_DIR)
    message(STATUS "Found liburing: ${URING_LIBRARY}")
    target_compile_definitions(elio INTERFACE ELIO_HAS_IO_URING=1)
    target_include_directories(elio INTERFACE ${URING_INCLUDE_DIR})
    target_link_libraries(elio INTERFACE fmt::fmt pthread uring)
else()
    message(STATUS "liburing not found, io_uring backend will be disabled")
    target_compile_definitions(elio INTERFACE ELIO_HAS_IO_URING=0)
    target_link_libraries(elio INTERFACE fmt::fmt pthread)
endif()

# Check for OpenSSL (optional, for TLS support)
find_package(OpenSSL QUIET)

# TLS support (standalone module)
if(ELIO_ENABLE_TLS AND OPENSSL_FOUND)
    message(STATUS "Found OpenSSL: ${OPENSSL_VERSION}")
    
    # Create elio_tls target with TLS support
    add_library(elio_tls INTERFACE)
    target_include_directories(elio_tls INTERFACE 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(elio_tls INTERFACE elio OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(elio_tls INTERFACE ELIO_HAS_TLS=1)
    message(STATUS "TLS support enabled")
elseif(ELIO_ENABLE_TLS AND NOT OPENSSL_FOUND)
    message(STATUS "OpenSSL not found, TLS support will be disabled")
elseif(NOT ELIO_ENABLE_TLS)
    message(STATUS "TLS support disabled (ELIO_ENABLE_TLS=OFF)")
endif()

# HTTP client/server support (depends on TLS)
if(ELIO_ENABLE_HTTP AND TARGET elio_tls)
    # Create elio_http target with HTTP support
    add_library(elio_http INTERFACE)
    target_include_directories(elio_http INTERFACE 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(elio_http INTERFACE elio_tls)
    target_compile_definitions(elio_http INTERFACE ELIO_HAS_HTTP=1)
    message(STATUS "HTTP support enabled")

    # nghttp2 for HTTP/2 support (requires OpenSSL for ALPN)
    if(ELIO_ENABLE_HTTP2)
        FetchContent_Declare(nghttp2
            GIT_REPOSITORY https://github.com/nghttp2/nghttp2.git
            GIT_TAG v1.64.0
        )
        # Configure nghttp2 build options
        set(ENABLE_LIB_ONLY ON CACHE BOOL "" FORCE)
        set(ENABLE_STATIC_LIB ON CACHE BOOL "" FORCE)
        set(ENABLE_SHARED_LIB OFF CACHE BOOL "" FORCE)
        set(ENABLE_DOC OFF CACHE BOOL "" FORCE)
        set(ENABLE_APP OFF CACHE BOOL "" FORCE)
        set(ENABLE_HPACK_TOOLS OFF CACHE BOOL "" FORCE)
        set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(ENABLE_FAILMALLOC OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(nghttp2)

        # Create elio_http2 target with HTTP/2 support
        add_library(elio_http2 INTERFACE)
        target_include_directories(elio_http2 INTERFACE 
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${nghttp2_SOURCE_DIR}/lib/includes>
            $<BUILD_INTERFACE:${nghttp2_BINARY_DIR}/lib/includes>
            $<INSTALL_INTERFACE:include>
        )
        target_link_libraries(elio_http2 INTERFACE
            elio_http
            $<BUILD_INTERFACE:nghttp2_static>
            $<INSTALL_INTERFACE:nghttp2::nghttp2_static>
        )
        target_compile_definitions(elio_http2 INTERFACE ELIO_HAS_HTTP2=1)
        message(STATUS "HTTP/2 support enabled via nghttp2")
    endif()
elseif(ELIO_ENABLE_HTTP AND NOT TARGET elio_tls)
    message(STATUS "HTTP support requires TLS, but TLS is not available")
elseif(NOT ELIO_ENABLE_HTTP)
    message(STATUS "HTTP support disabled (ELIO_ENABLE_HTTP=OFF)")
endif()

# RDMA-Core abstraction (header-only, optional)
#
# Elio's RDMA layer is a thin protocol-agnostic abstraction over RDMA-Core
# verbs. It does NOT depend on libibverbs or librdmacm itself — users plug
# in their own backend (via template traits or the polymorphic_backend
# pure-virtual class) and link those libraries themselves. This keeps
# Elio header-only and lets consumers choose between libibverbs, a mock
# backend (for tests), or any other implementation that satisfies the
# elio::rdma::backend_traits concept.
#
# A separate elio_rdma_cm target (Stage 8) will provide an optional
# librdmacm-backed connection helper; it is not built by this option.
if(ELIO_ENABLE_RDMA)
    add_library(elio_rdma INTERFACE)
    target_include_directories(elio_rdma INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(elio_rdma INTERFACE elio)
    target_compile_definitions(elio_rdma INTERFACE ELIO_HAS_RDMA=1)
    message(STATUS "RDMA core abstraction enabled (header-only; bring your own ibverbs backend)")
else()
    message(STATUS "RDMA support disabled (ELIO_ENABLE_RDMA=OFF)")
endif()

# Optional librdmacm-backed CM helper (Stage 8).
#
# Adds the elio_rdma_cm INTERFACE target that links librdmacm and
# exposes a TCP-style listen / connect surface that hands an
# established ibv_qp* back to elio::rdma::connection for the data
# path. Unlike elio_rdma itself, this DOES link a system library —
# the user opts in by setting ELIO_ENABLE_RDMA_CM=ON. We try
# pkg-config first, then fall back to find_library so packages that
# ship librdmacm without a .pc file still work.
if(ELIO_ENABLE_RDMA_CM)
    if(NOT TARGET elio_rdma)
        message(FATAL_ERROR
            "ELIO_ENABLE_RDMA_CM=ON requires ELIO_ENABLE_RDMA=ON")
    endif()
    find_package(PkgConfig QUIET)
    set(_RDMACM_LIB "")
    set(_RDMACM_INC "")
    if(PkgConfig_FOUND)
        pkg_check_modules(RDMACM QUIET librdmacm)
        if(RDMACM_FOUND)
            set(_RDMACM_LIB "${RDMACM_LIBRARIES}")
            set(_RDMACM_INC "${RDMACM_INCLUDE_DIRS}")
        endif()
    endif()
    if(NOT _RDMACM_LIB)
        find_library(_RDMACM_LIB_FALLBACK rdmacm)
        find_path(_RDMACM_INC_FALLBACK rdma/rdma_cma.h)
        if(_RDMACM_LIB_FALLBACK AND _RDMACM_INC_FALLBACK)
            set(_RDMACM_LIB "${_RDMACM_LIB_FALLBACK}")
            set(_RDMACM_INC "${_RDMACM_INC_FALLBACK}")
        endif()
    endif()
    if(NOT _RDMACM_LIB)
        message(FATAL_ERROR
            "ELIO_ENABLE_RDMA_CM=ON but librdmacm was not found "
            "(checked pkg-config librdmacm and find_library(rdmacm)). "
            "Install librdmacm-dev or set CMAKE_PREFIX_PATH.")
    endif()
    add_library(elio_rdma_cm INTERFACE)
    target_include_directories(elio_rdma_cm INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    if(_RDMACM_INC)
        target_include_directories(elio_rdma_cm INTERFACE ${_RDMACM_INC})
    endif()
    target_link_libraries(elio_rdma_cm INTERFACE elio_rdma ${_RDMACM_LIB})
    target_compile_definitions(elio_rdma_cm INTERFACE ELIO_HAS_RDMA_CM=1)
    message(STATUS "RDMA CM helper enabled (linking ${_RDMACM_LIB})")
endif()

# Optional libibverbs lookup, shared by ELIO_ENABLE_RDMA_IBVERBS
# (the public backend / cq_drain / endpoint module) and
# ELIO_ENABLE_RDMA_IBVERBS_TESTS (the integration test binary).
# Same detection pattern as librdmacm: pkg-config first,
# find_library fallback.
set(_IBVERBS_LIB "")
set(_IBVERBS_INC "")
if(ELIO_ENABLE_RDMA_IBVERBS OR ELIO_ENABLE_RDMA_IBVERBS_TESTS)
    if(NOT TARGET elio_rdma)
        message(FATAL_ERROR
            "ELIO_ENABLE_RDMA_IBVERBS / _TESTS=ON requires ELIO_ENABLE_RDMA=ON")
    endif()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(IBVERBS QUIET libibverbs)
        if(IBVERBS_FOUND)
            set(_IBVERBS_LIB "${IBVERBS_LIBRARIES}")
            set(_IBVERBS_INC "${IBVERBS_INCLUDE_DIRS}")
        endif()
    endif()
    if(NOT _IBVERBS_LIB)
        find_library(_IBVERBS_LIB_FALLBACK ibverbs)
        find_path(_IBVERBS_INC_FALLBACK infiniband/verbs.h)
        if(_IBVERBS_LIB_FALLBACK AND _IBVERBS_INC_FALLBACK)
            set(_IBVERBS_LIB "${_IBVERBS_LIB_FALLBACK}")
            set(_IBVERBS_INC "${_IBVERBS_INC_FALLBACK}")
        endif()
    endif()
    if(NOT _IBVERBS_LIB)
        message(FATAL_ERROR
            "ELIO_ENABLE_RDMA_IBVERBS / _TESTS=ON but libibverbs was not found.")
    endif()
    set(ELIO_IBVERBS_LIB "${_IBVERBS_LIB}" CACHE INTERNAL
        "libibverbs link spec")
    set(ELIO_IBVERBS_INC "${_IBVERBS_INC}" CACHE INTERNAL
        "libibverbs include dir")
endif()

# elio_rdma_ibverbs INTERFACE target: public libibverbs-backed
# backend + cq_drain helper + endpoint wrapper (S12-S13).
if(ELIO_ENABLE_RDMA_IBVERBS)
    add_library(elio_rdma_ibverbs INTERFACE)
    target_include_directories(elio_rdma_ibverbs INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    if(ELIO_IBVERBS_INC)
        target_include_directories(elio_rdma_ibverbs INTERFACE ${ELIO_IBVERBS_INC})
    endif()
    target_link_libraries(elio_rdma_ibverbs INTERFACE elio_rdma ${ELIO_IBVERBS_LIB})
    target_compile_definitions(elio_rdma_ibverbs INTERFACE ELIO_HAS_RDMA_IBVERBS=1)
    # If the CM helper is also enabled, the endpoint wrapper becomes
    # active and needs to pull in librdmacm transitively.
    if(TARGET elio_rdma_cm)
        target_link_libraries(elio_rdma_ibverbs INTERFACE elio_rdma_cm)
    endif()
    message(STATUS "RDMA ibverbs module enabled (linking ${ELIO_IBVERBS_LIB})")
endif()
if(ELIO_ENABLE_RDMA_IBVERBS_TESTS)
    message(STATUS "RDMA ibverbs integration tests enabled "
                   "(linking ${ELIO_IBVERBS_LIB})")
endif()

if(ELIO_ENABLE_RDMA_CUDA)
    if(NOT TARGET elio_rdma_ibverbs)
        message(FATAL_ERROR
            "ELIO_ENABLE_RDMA_CUDA=ON requires ELIO_ENABLE_RDMA_IBVERBS=ON")
    endif()
    find_package(CUDAToolkit REQUIRED)
    add_library(elio_rdma_cuda INTERFACE)
    target_include_directories(elio_rdma_cuda INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(elio_rdma_cuda INTERFACE
        elio_rdma_ibverbs CUDA::cudart)
    target_compile_definitions(elio_rdma_cuda INTERFACE ELIO_HAS_RDMA_CUDA=1)
    message(STATUS "RDMA CUDA GPUDirect helpers enabled")
endif()

# Testing
if(ELIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# TCP loopback benchmarks (Elio vs libuv vs Asio)
option(ELIO_BUILD_TCP_BENCHMARKS
    "Build TCP loopback benchmarks comparing Elio vs libuv vs standalone Asio" OFF)

if(ELIO_BUILD_TCP_BENCHMARKS)
    # libuv: prefer system installation
    find_path(LIBUV_INCLUDE_DIR uv.h)
    find_library(LIBUV_LIBRARY NAMES uv uv_a)
    if(LIBUV_INCLUDE_DIR AND LIBUV_LIBRARY)
        message(STATUS "TCP benchmark: libuv found at ${LIBUV_LIBRARY}")
    else()
        message(STATUS "TCP benchmark: libuv not found — bench_tcp_libuv will not be built")
    endif()

    # Standalone Asio: fetch via FetchContent (header-only, no Boost needed)
    include(FetchContent)
    FetchContent_Declare(asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG        asio-1-30-2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(asio)
    # Asio is header-only — create an INTERFACE target
    if(NOT TARGET asio)
        add_library(asio INTERFACE)
        target_include_directories(asio INTERFACE
            ${asio_SOURCE_DIR}/asio/include)
        target_compile_definitions(asio INTERFACE ASIO_STANDALONE)
    endif()
    message(STATUS "TCP benchmark: standalone Asio enabled")
endif()

# Examples
if(ELIO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Installation
install(DIRECTORY include/ DESTINATION include)

set(ELIO_EXPORT_TARGETS elio)
if(TARGET elio_tls)
    list(APPEND ELIO_EXPORT_TARGETS elio_tls)
endif()
if(TARGET elio_http)
    list(APPEND ELIO_EXPORT_TARGETS elio_http)
endif()
if(TARGET elio_http2)
    list(APPEND ELIO_EXPORT_TARGETS elio_http2)
endif()
if(TARGET elio_rdma)
    list(APPEND ELIO_EXPORT_TARGETS elio_rdma)
endif()
if(TARGET elio_rdma_cm)
    list(APPEND ELIO_EXPORT_TARGETS elio_rdma_cm)
endif()
if(TARGET elio_rdma_ibverbs)
    list(APPEND ELIO_EXPORT_TARGETS elio_rdma_ibverbs)
endif()
if(TARGET elio_rdma_cuda)
    list(APPEND ELIO_EXPORT_TARGETS elio_rdma_cuda)
endif()

set(ELIO_PACKAGE_NEEDS_OPENSSL OFF)
if(TARGET elio_tls)
    set(ELIO_PACKAGE_NEEDS_OPENSSL ON)
endif()

set(ELIO_PACKAGE_NEEDS_NGHTTP2 OFF)
if(TARGET elio_http2)
    set(ELIO_PACKAGE_NEEDS_NGHTTP2 ON)
endif()

set(ELIO_PACKAGE_NEEDS_CUDATOOLKIT OFF)
if(TARGET elio_rdma_cuda)
    set(ELIO_PACKAGE_NEEDS_CUDATOOLKIT ON)
endif()

install(TARGETS ${ELIO_EXPORT_TARGETS} EXPORT ElioTargets)
install(EXPORT ElioTargets
    FILE ElioTargets.cmake
    NAMESPACE Elio::
    DESTINATION lib/cmake/Elio
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ElioConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ElioConfig.cmake
    INSTALL_DESTINATION lib/cmake/Elio
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ElioConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ElioConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ElioConfigVersion.cmake
    DESTINATION lib/cmake/Elio
)

# Install debug tools
install(PROGRAMS
    tools/elio-pstack
    DESTINATION bin
)
install(FILES
    tools/elio-gdb.py
    tools/elio_lldb.py
    tools/elio-lldb.py
    DESTINATION share/elio
)
