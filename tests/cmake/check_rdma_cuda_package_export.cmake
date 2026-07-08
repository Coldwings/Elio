if(NOT DEFINED ELIO_SOURCE_DIR)
    message(FATAL_ERROR "ELIO_SOURCE_DIR is required")
endif()

if(NOT DEFINED ELIO_BINARY_DIR)
    message(FATAL_ERROR "ELIO_BINARY_DIR is required")
endif()

file(READ "${ELIO_SOURCE_DIR}/CMakeLists.txt" _elio_root_cmake)

foreach(_required_text IN ITEMS
    "list(APPEND ELIO_EXPORT_TARGETS elio_rdma_cuda)"
    "set(ELIO_PACKAGE_NEEDS_CUDATOOLKIT OFF)"
    "set(ELIO_PACKAGE_NEEDS_CUDATOOLKIT ON)"
)
    string(FIND "${_elio_root_cmake}" "${_required_text}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "Missing package export metadata: ${_required_text}")
    endif()
endforeach()

include(CMakePackageConfigHelpers)

file(MAKE_DIRECTORY "${ELIO_BINARY_DIR}")
set(ELIO_PACKAGE_NEEDS_OPENSSL OFF)
set(ELIO_PACKAGE_NEEDS_NGHTTP2 OFF)
set(ELIO_PACKAGE_NEEDS_CUDATOOLKIT ON)

configure_package_config_file(
    "${ELIO_SOURCE_DIR}/cmake/ElioConfig.cmake.in"
    "${ELIO_BINARY_DIR}/ElioConfig.cmake"
    INSTALL_DESTINATION lib/cmake/Elio
)

file(READ "${ELIO_BINARY_DIR}/ElioConfig.cmake" _elio_config)
string(FIND "${_elio_config}" "find_dependency(CUDAToolkit REQUIRED)" _cuda_pos)
if(_cuda_pos EQUAL -1)
    message(FATAL_ERROR "ElioConfig.cmake does not restore CUDAToolkit")
endif()

string(FIND "${_elio_config}" "include(\${CMAKE_CURRENT_LIST_DIR}/ElioTargets.cmake)" _targets_pos)
if(_targets_pos EQUAL -1)
    message(FATAL_ERROR "ElioConfig.cmake does not load ElioTargets.cmake")
endif()

if(NOT _cuda_pos LESS _targets_pos)
    message(FATAL_ERROR "CUDAToolkit must be restored before ElioTargets.cmake is loaded")
endif()
