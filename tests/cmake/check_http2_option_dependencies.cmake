if(NOT DEFINED ELIO_SOURCE_DIR)
    message(FATAL_ERROR "ELIO_SOURCE_DIR is required")
endif()

if(NOT DEFINED ELIO_BINARY_DIR)
    message(FATAL_ERROR "ELIO_BINARY_DIR is required")
endif()

set(_probe_build_dir "${ELIO_BINARY_DIR}/http2_without_http")
file(REMOVE_RECURSE "${_probe_build_dir}")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${ELIO_SOURCE_DIR}"
        -B "${_probe_build_dir}"
        -DELIO_BUILD_TESTS=OFF
        -DELIO_BUILD_EXAMPLES=OFF
        -DELIO_ENABLE_TLS=OFF
        -DELIO_ENABLE_HTTP=OFF
        -DELIO_ENABLE_HTTP2=ON
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_stdout
    ERROR_VARIABLE _configure_stderr
)

if(_configure_result EQUAL 0)
    message(FATAL_ERROR
        "Expected ELIO_ENABLE_HTTP2=ON with ELIO_ENABLE_HTTP=OFF to fail, "
        "but configure succeeded.")
endif()

set(_configure_output "${_configure_stdout}\n${_configure_stderr}")
if(NOT _configure_output MATCHES
   "ELIO_ENABLE_HTTP2=ON requires ELIO_ENABLE_HTTP=ON")
    message(FATAL_ERROR
        "Configure failed for the wrong reason.\n"
        "stdout:\n${_configure_stdout}\n"
        "stderr:\n${_configure_stderr}")
endif()
