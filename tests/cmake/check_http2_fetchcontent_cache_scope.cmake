if(NOT DEFINED ELIO_SOURCE_DIR)
    message(FATAL_ERROR "ELIO_SOURCE_DIR is required")
endif()

if(NOT DEFINED ELIO_BINARY_DIR)
    message(FATAL_ERROR "ELIO_BINARY_DIR is required")
endif()

if(NOT DEFINED ELIO_PARENT_BINARY_DIR)
    message(FATAL_ERROR "ELIO_PARENT_BINARY_DIR is required")
endif()

set(_probe_root "${ELIO_BINARY_DIR}")
set(_consumer_source_dir "${_probe_root}/consumer-src")
set(_consumer_build_dir "${_probe_root}/consumer-build")
if(DEFINED ELIO_PARENT_FMT_SOURCE_DIR)
    set(_parent_fmt_source "${ELIO_PARENT_FMT_SOURCE_DIR}")
else()
    set(_parent_fmt_source "${ELIO_PARENT_BINARY_DIR}/_deps/fmt-src")
endif()
if(DEFINED ELIO_PARENT_NGHTTP2_SOURCE_DIR)
    set(_parent_nghttp2_source "${ELIO_PARENT_NGHTTP2_SOURCE_DIR}")
else()
    set(_parent_nghttp2_source "${ELIO_PARENT_BINARY_DIR}/_deps/nghttp2-src")
endif()

if(NOT EXISTS "${_parent_fmt_source}/CMakeLists.txt")
    message(FATAL_ERROR
        "Parent fmt FetchContent source is required at ${_parent_fmt_source}")
endif()

if(NOT EXISTS "${_parent_nghttp2_source}/CMakeLists.txt")
    message(FATAL_ERROR
        "Parent nghttp2 FetchContent source is required at ${_parent_nghttp2_source}")
endif()

file(REMOVE_RECURSE "${_probe_root}")
file(MAKE_DIRECTORY "${_consumer_source_dir}")

set(PARENT_FMT_SOURCE "${_parent_fmt_source}")
set(PARENT_NGHTTP2_SOURCE "${_parent_nghttp2_source}")

file(WRITE "${_consumer_source_dir}/CMakeLists.txt.in" [=[
cmake_minimum_required(VERSION 3.20)
project(elio_http2_cache_scope_consumer LANGUAGES CXX)

include(FetchContent)

set(BUILD_TESTING ON CACHE BOOL "consumer test policy" FORCE)
set(BUILD_SHARED_LIBS ON CACHE BOOL "consumer shared-library policy" FORCE)
set(BUILD_STATIC_LIBS OFF CACHE BOOL "consumer static-library policy" FORCE)

set(FETCHCONTENT_SOURCE_DIR_FMT "@PARENT_FMT_SOURCE@" CACHE PATH "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_NGHTTP2 "@PARENT_NGHTTP2_SOURCE@" CACHE PATH "" FORCE)

FetchContent_Declare(elio SOURCE_DIR "@ELIO_SOURCE_DIR@")
set(ELIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ELIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ELIO_ENABLE_TLS ON CACHE BOOL "" FORCE)
set(ELIO_ENABLE_HTTP ON CACHE BOOL "" FORCE)
set(ELIO_ENABLE_HTTP2 ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(elio)

if(NOT TARGET elio_http2)
    message(FATAL_ERROR "Expected elio_http2 target to be available")
endif()

if(NOT TARGET nghttp2_static)
    message(FATAL_ERROR "Expected nghttp2_static target to be available")
endif()

get_property(_build_testing CACHE BUILD_TESTING PROPERTY VALUE)
get_property(_build_shared_libs CACHE BUILD_SHARED_LIBS PROPERTY VALUE)
get_property(_build_static_libs CACHE BUILD_STATIC_LIBS PROPERTY VALUE)
get_property(_cmake_build_type CACHE CMAKE_BUILD_TYPE PROPERTY VALUE)

if(NOT _build_testing)
    message(FATAL_ERROR "Elio HTTP/2 rewrote consumer BUILD_TESTING")
endif()

if(NOT _build_shared_libs)
    message(FATAL_ERROR "Elio HTTP/2 rewrote consumer BUILD_SHARED_LIBS")
endif()

if(_build_static_libs)
    message(FATAL_ERROR "Elio HTTP/2 rewrote consumer BUILD_STATIC_LIBS")
endif()

if(NOT "${_cmake_build_type}" STREQUAL "")
    message(FATAL_ERROR "Elio HTTP/2 rewrote consumer CMAKE_BUILD_TYPE")
endif()
]=])

configure_file(
    "${_consumer_source_dir}/CMakeLists.txt.in"
    "${_consumer_source_dir}/CMakeLists.txt"
    @ONLY
)

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${_consumer_source_dir}"
        -B "${_consumer_build_dir}"
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_stdout
    ERROR_VARIABLE _configure_stderr
)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to configure HTTP/2 cache-scope consumer\n"
        "stdout:\n${_configure_stdout}\n"
        "stderr:\n${_configure_stderr}")
endif()
