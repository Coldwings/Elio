if(NOT DEFINED ELIO_SOURCE_DIR)
    message(FATAL_ERROR "ELIO_SOURCE_DIR is required")
endif()

if(NOT DEFINED ELIO_BINARY_DIR)
    message(FATAL_ERROR "ELIO_BINARY_DIR is required")
endif()

file(READ "${ELIO_SOURCE_DIR}/CMakeLists.txt" _elio_root_cmake)

foreach(_required_text IN ITEMS
    "add_library(rdmacm::rdmacm UNKNOWN IMPORTED)"
    "add_library(ibverbs::ibverbs UNKNOWN IMPORTED)"
    "set(ELIO_PACKAGE_NEEDS_RDMACM OFF)"
    "set(ELIO_PACKAGE_NEEDS_RDMACM ON)"
    "set(ELIO_PACKAGE_NEEDS_IBVERBS OFF)"
    "set(ELIO_PACKAGE_NEEDS_IBVERBS ON)"
)
    string(FIND "${_elio_root_cmake}" "${_required_text}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "Missing RDMA package metadata: ${_required_text}")
    endif()
endforeach()

string(REGEX MATCH
    "target_link_libraries\\([ \t\r\n]*elio_rdma_cm[ \t\r\n]+INTERFACE[^)]*rdmacm::rdmacm"
    _elio_rdmacm_link "${_elio_root_cmake}")
if(NOT _elio_rdmacm_link)
    message(FATAL_ERROR
        "Missing rdmacm::rdmacm in elio_rdma_cm's INTERFACE link libraries")
endif()

string(REGEX MATCH
    "target_link_libraries\\([ \t\r\n]*elio_rdma_ibverbs[ \t\r\n]+INTERFACE[^)]*ibverbs::ibverbs"
    _elio_ibverbs_link "${_elio_root_cmake}")
if(NOT _elio_ibverbs_link)
    message(FATAL_ERROR
        "Missing ibverbs::ibverbs in elio_rdma_ibverbs's INTERFACE link libraries")
endif()

include(CMakePackageConfigHelpers)

file(MAKE_DIRECTORY "${ELIO_BINARY_DIR}")
set(ELIO_PACKAGE_NEEDS_OPENSSL OFF)
set(ELIO_PACKAGE_NEEDS_NGHTTP2 OFF)
set(ELIO_PACKAGE_NEEDS_LIBURING OFF)
set(ELIO_PACKAGE_NEEDS_RDMACM ON)
set(ELIO_PACKAGE_NEEDS_IBVERBS ON)
set(ELIO_PACKAGE_NEEDS_CUDATOOLKIT OFF)

configure_package_config_file(
    "${ELIO_SOURCE_DIR}/cmake/ElioConfig.cmake.in"
    "${ELIO_BINARY_DIR}/ElioConfig.cmake"
    INSTALL_DESTINATION lib/cmake/Elio
)

file(READ "${ELIO_BINARY_DIR}/ElioConfig.cmake" _elio_config)
string(FIND "${_elio_config}"
    "find_library(ELIO_RDMACM_LIBRARY NAMES rdmacm)" _find_rdmacm_lib_pos)
string(FIND "${_elio_config}"
    "find_path(ELIO_RDMACM_INCLUDE_DIR NAMES rdma/rdma_cma.h)" _find_rdmacm_include_pos)
string(FIND "${_elio_config}"
    "add_library(rdmacm::rdmacm UNKNOWN IMPORTED)" _rdmacm_target_pos)
string(FIND "${_elio_config}"
    "find_library(ELIO_IBVERBS_LIBRARY NAMES ibverbs)" _find_ibverbs_lib_pos)
string(FIND "${_elio_config}"
    "find_path(ELIO_IBVERBS_INCLUDE_DIR NAMES infiniband/verbs.h)" _find_ibverbs_include_pos)
string(FIND "${_elio_config}"
    "add_library(ibverbs::ibverbs UNKNOWN IMPORTED)" _ibverbs_target_pos)
string(FIND "${_elio_config}"
    "include(\${CMAKE_CURRENT_LIST_DIR}/ElioTargets.cmake)" _targets_pos)

if(_find_rdmacm_lib_pos EQUAL -1 OR _find_rdmacm_include_pos EQUAL -1)
    message(FATAL_ERROR "ElioConfig.cmake does not restore librdmacm")
endif()

if(_rdmacm_target_pos EQUAL -1)
    message(FATAL_ERROR "ElioConfig.cmake does not create rdmacm::rdmacm")
endif()

if(_find_ibverbs_lib_pos EQUAL -1 OR _find_ibverbs_include_pos EQUAL -1)
    message(FATAL_ERROR "ElioConfig.cmake does not restore libibverbs")
endif()

if(_ibverbs_target_pos EQUAL -1)
    message(FATAL_ERROR "ElioConfig.cmake does not create ibverbs::ibverbs")
endif()

if(_targets_pos EQUAL -1)
    message(FATAL_ERROR "ElioConfig.cmake does not load ElioTargets.cmake")
endif()

if(NOT _find_rdmacm_lib_pos LESS _targets_pos
   OR NOT _find_rdmacm_include_pos LESS _targets_pos
   OR NOT _rdmacm_target_pos LESS _targets_pos
   OR NOT _find_ibverbs_lib_pos LESS _targets_pos
   OR NOT _find_ibverbs_include_pos LESS _targets_pos
   OR NOT _ibverbs_target_pos LESS _targets_pos)
    message(FATAL_ERROR
        "RDMA dependencies must be restored before ElioTargets.cmake is loaded")
endif()
