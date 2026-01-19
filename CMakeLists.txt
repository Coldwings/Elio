cmake_minimum_required(VERSION 3.20)
project(Elio VERSION 0.1.0 LANGUAGES CXX)

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

# Catch2 for testing
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.0
)
FetchContent_MakeAvailable(Catch2)

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

# Check for OpenSSL (optional, for TLS/HTTPS support)
find_package(OpenSSL QUIET)
if(OPENSSL_FOUND)
    message(STATUS "Found OpenSSL: ${OPENSSL_VERSION}")
    
    # Create elio_http target with TLS support
    add_library(elio_http INTERFACE)
    target_include_directories(elio_http INTERFACE 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(elio_http INTERFACE elio OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(elio_http INTERFACE ELIO_HAS_TLS=1)

    # nghttp2 for HTTP/2 support (requires OpenSSL for ALPN)
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
else()
    message(STATUS "OpenSSL not found, TLS/HTTPS and HTTP/2 support will be disabled")
endif()

# Testing
enable_testing()
add_subdirectory(tests)

# Examples
add_subdirectory(examples)

# Installation
install(DIRECTORY include/ DESTINATION include)
install(TARGETS elio EXPORT ElioTargets)
install(EXPORT ElioTargets
    FILE ElioTargets.cmake
    NAMESPACE Elio::
    DESTINATION lib/cmake/Elio
)
