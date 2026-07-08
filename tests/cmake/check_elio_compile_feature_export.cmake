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
set(_elio_build_dir "${_probe_root}/elio-build")
set(_elio_install_dir "${_probe_root}/elio-install")
set(_consumer_source_dir "${_probe_root}/consumer-src")
set(_consumer_build_dir "${_probe_root}/consumer-build")
set(_targets_file "${_elio_install_dir}/lib/cmake/Elio/ElioTargets.cmake")

file(REMOVE_RECURSE "${_probe_root}")
file(MAKE_DIRECTORY "${_consumer_source_dir}")

file(WRITE "${_consumer_source_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.20)
project(elio_compile_feature_consumer LANGUAGES CXX)

find_package(Elio REQUIRED CONFIG PATHS "@ELIO_INSTALL_DIR@" NO_DEFAULT_PATH)

add_executable(elio_compile_feature_consumer main.cpp)
target_link_libraries(elio_compile_feature_consumer PRIVATE Elio::elio)
]=])
file(WRITE "${_consumer_source_dir}/main.cpp" [=[
#include <concepts>
#include <coroutine>
#include <elio/coro/task.hpp>
#include <elio/net/stream.hpp>

static_assert(__cplusplus >= 202002L,
    "Elio::elio must propagate a C++20 compile requirement");

int main() {
    elio::net::stream stream;
    return (stream.is_connected() || stream.is_secure()) ? 1 : 0;
}
]=])
file(READ "${_consumer_source_dir}/CMakeLists.txt" _consumer_cmake)
string(REPLACE "@ELIO_INSTALL_DIR@" "${_elio_install_dir}" _consumer_cmake
    "${_consumer_cmake}")
file(WRITE "${_consumer_source_dir}/CMakeLists.txt" "${_consumer_cmake}")

set(_configure_args
    -S ${ELIO_SOURCE_DIR}
    -B ${_elio_build_dir}
    -DCMAKE_INSTALL_PREFIX=${_elio_install_dir}
    -DELIO_BUILD_TESTS=OFF
    -DELIO_BUILD_EXAMPLES=OFF
    -DELIO_ENABLE_TLS=OFF
    -DELIO_ENABLE_HTTP=OFF
    -DELIO_ENABLE_HTTP2=OFF
)

set(_parent_fmt_source "${ELIO_PARENT_BINARY_DIR}/_deps/fmt-src")
if(EXISTS "${_parent_fmt_source}/CMakeLists.txt")
    list(APPEND _configure_args
        -DFETCHCONTENT_SOURCE_DIR_FMT=${_parent_fmt_source})
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_configure_args}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_stdout
    ERROR_VARIABLE _configure_stderr
)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to configure nested Elio package build\n"
        "stdout:\n${_configure_stdout}\n"
        "stderr:\n${_configure_stderr}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_elio_build_dir}" --parallel 2
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_stdout
    ERROR_VARIABLE _build_stderr
)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to build nested Elio package build\n"
        "stdout:\n${_build_stdout}\n"
        "stderr:\n${_build_stderr}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${_elio_build_dir}"
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_stdout
    ERROR_VARIABLE _install_stderr
)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to install nested Elio package build\n"
        "stdout:\n${_install_stdout}\n"
        "stderr:\n${_install_stderr}")
endif()

if(NOT EXISTS "${_targets_file}")
    message(FATAL_ERROR "Missing installed ElioTargets.cmake")
endif()

file(READ "${_targets_file}" _targets_contents)
string(FIND "${_targets_contents}" "cxx_std_20" _exported_feature_pos)
if(_exported_feature_pos EQUAL -1)
    message(FATAL_ERROR
        "Installed ElioTargets.cmake does not export cxx_std_20 on Elio::elio")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${_consumer_source_dir}"
        -B "${_consumer_build_dir}"
        -Dfmt_DIR=${_elio_build_dir}/_deps/fmt-build
    RESULT_VARIABLE _consumer_configure_result
    OUTPUT_VARIABLE _consumer_configure_stdout
    ERROR_VARIABLE _consumer_configure_stderr
)
if(NOT _consumer_configure_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to configure downstream consumer without setting CMAKE_CXX_STANDARD\n"
        "stdout:\n${_consumer_configure_stdout}\n"
        "stderr:\n${_consumer_configure_stderr}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_consumer_build_dir}" --parallel 2
    RESULT_VARIABLE _consumer_build_result
    OUTPUT_VARIABLE _consumer_build_stdout
    ERROR_VARIABLE _consumer_build_stderr
)
if(NOT _consumer_build_result EQUAL 0)
    message(FATAL_ERROR
        "Downstream consumer did not build with Elio::elio's exported C++20 requirement and TCP-only stream header\n"
        "stdout:\n${_consumer_build_stdout}\n"
        "stderr:\n${_consumer_build_stderr}")
endif()
