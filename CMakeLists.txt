cmake_minimum_required(VERSION 3.20)
project(Elio VERSION 0.3.0 LANGUAGES CXX)

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

# Compiler flags for our code only (not dependencies)
target_compile_options(elio INTERFACE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
)

# Dependencies via FetchContent
include(FetchContent)

# fmtlib for logging
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1
)
FetchContent_MakeAvailable(fmt)

# Catch2 for testing (only if tests are enabled)
if(ELIO_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.0
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
    target_link_libraries(elio INTERFACE fmt::fmt pthread ${URING_LIBRARY})
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
        target_link_libraries(elio_http2 INTERFACE elio_http nghttp2_static)
        target_compile_definitions(elio_http2 INTERFACE ELIO_HAS_HTTP2=1)
        message(STATUS "HTTP/2 support enabled via nghttp2")
    endif()
elseif(ELIO_ENABLE_HTTP AND NOT TARGET elio_tls)
    message(STATUS "HTTP support requires TLS, but TLS is not available")
elseif(NOT ELIO_ENABLE_HTTP)
    message(STATUS "HTTP support disabled (ELIO_ENABLE_HTTP=OFF)")
endif()

# Testing
if(ELIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Examples
if(ELIO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Installation
install(DIRECTORY include/ DESTINATION include)
install(TARGETS elio EXPORT ElioTargets)
install(EXPORT ElioTargets
    FILE ElioTargets.cmake
    NAMESPACE Elio::
    DESTINATION lib/cmake/Elio
)

# Install debug tools
install(PROGRAMS
    tools/elio-pstack
    DESTINATION bin
)
install(FILES
    tools/elio-gdb.py
    tools/elio-lldb.py
    DESTINATION share/elio
)
