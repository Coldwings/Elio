if(NOT DEFINED ELIO_SOURCE_DIR)
    message(FATAL_ERROR "ELIO_SOURCE_DIR is required")
endif()

if(NOT DEFINED ELIO_BINARY_DIR)
    message(FATAL_ERROR "ELIO_BINARY_DIR is required")
endif()

set(_probe_build_dir "${ELIO_BINARY_DIR}/freebsd")
file(REMOVE_RECURSE "${_probe_build_dir}")

set(_configure_args
    -S "${ELIO_SOURCE_DIR}"
    -B "${_probe_build_dir}"
    -DCMAKE_SYSTEM_NAME=FreeBSD
    -DCMAKE_CXX_COMPILER_WORKS=TRUE
    -DELIO_BUILD_TESTS=OFF
    -DELIO_BUILD_EXAMPLES=OFF
    -DELIO_ENABLE_TLS=OFF
    -DELIO_ENABLE_HTTP=OFF
    -DELIO_ENABLE_HTTP2=OFF
)
if(DEFINED ELIO_CMAKE_GENERATOR AND NOT ELIO_CMAKE_GENERATOR STREQUAL "")
    list(APPEND _configure_args -G "${ELIO_CMAKE_GENERATOR}")
endif()
if(DEFINED ELIO_CXX_COMPILER AND NOT ELIO_CXX_COMPILER STREQUAL "")
    list(APPEND _configure_args
        "-DCMAKE_CXX_COMPILER=${ELIO_CXX_COMPILER}")
endif()
if(DEFINED ELIO_FMT_SOURCE_DIR AND
   EXISTS "${ELIO_FMT_SOURCE_DIR}/CMakeLists.txt")
    list(APPEND _configure_args
        -DFETCHCONTENT_SOURCE_DIR_FMT=${ELIO_FMT_SOURCE_DIR})
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_configure_args}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_stdout
    ERROR_VARIABLE _configure_stderr
)

if(_configure_result EQUAL 0)
    message(FATAL_ERROR
        "Expected a FreeBSD target configuration to be rejected, but it "
        "succeeded.")
endif()

set(_configure_output "${_configure_stdout}\n${_configure_stderr}")
if(NOT _configure_output MATCHES "Elio currently only supports Linux")
    message(FATAL_ERROR
        "FreeBSD configuration failed for the wrong reason.\n"
        "stdout:\n${_configure_stdout}\n"
        "stderr:\n${_configure_stderr}")
endif()
