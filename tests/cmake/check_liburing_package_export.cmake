if(NOT DEFINED ELIO_SOURCE_DIR)
    message(FATAL_ERROR "ELIO_SOURCE_DIR is required")
endif()

if(NOT DEFINED ELIO_BINARY_DIR)
    message(FATAL_ERROR "ELIO_BINARY_DIR is required")
endif()

file(READ "${ELIO_SOURCE_DIR}/CMakeLists.txt" _elio_root_cmake)

foreach(_required_text IN ITEMS
    "add_library(liburing::liburing UNKNOWN IMPORTED)"
    "set(ELIO_PACKAGE_NEEDS_LIBURING OFF)"
    "set(ELIO_PACKAGE_NEEDS_LIBURING ON)"
)
    string(FIND "${_elio_root_cmake}" "${_required_text}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "Missing liburing package metadata: ${_required_text}")
    endif()
endforeach()

string(REGEX MATCH
    "target_link_libraries\\([ \t\r\n]*elio[ \t\r\n]+INTERFACE[^)]*liburing::liburing"
    _elio_liburing_link "${_elio_root_cmake}")
if(NOT _elio_liburing_link)
    message(FATAL_ERROR
        "Missing liburing::liburing in elio's INTERFACE link libraries")
endif()

include(CMakePackageConfigHelpers)

file(MAKE_DIRECTORY "${ELIO_BINARY_DIR}")
set(ELIO_PACKAGE_NEEDS_OPENSSL OFF)
set(ELIO_PACKAGE_NEEDS_NGHTTP2 OFF)
set(ELIO_PACKAGE_NEEDS_LIBURING ON)
set(ELIO_PACKAGE_NEEDS_RDMACM OFF)
set(ELIO_PACKAGE_NEEDS_IBVERBS OFF)
set(ELIO_PACKAGE_NEEDS_CUDATOOLKIT OFF)

configure_package_config_file(
    "${ELIO_SOURCE_DIR}/cmake/ElioConfig.cmake.in"
    "${ELIO_BINARY_DIR}/ElioConfig.cmake"
    INSTALL_DESTINATION lib/cmake/Elio
)

file(READ "${ELIO_BINARY_DIR}/ElioConfig.cmake" _elio_config)
string(FIND "${_elio_config}"
    "find_library(ELIO_LIBURING_LIBRARY NAMES uring)" _find_lib_pos)
string(FIND "${_elio_config}"
    "find_path(ELIO_LIBURING_INCLUDE_DIR NAMES liburing.h)" _find_include_pos)
string(FIND "${_elio_config}"
    "add_library(liburing::liburing UNKNOWN IMPORTED)" _target_pos)
string(FIND "${_elio_config}"
    "include(\${CMAKE_CURRENT_LIST_DIR}/ElioTargets.cmake)" _targets_pos)

if(_find_lib_pos EQUAL -1 OR _find_include_pos EQUAL -1)
    message(FATAL_ERROR "ElioConfig.cmake does not restore liburing")
endif()

if(_target_pos EQUAL -1)
    message(FATAL_ERROR "ElioConfig.cmake does not create liburing::liburing")
endif()

if(_targets_pos EQUAL -1)
    message(FATAL_ERROR "ElioConfig.cmake does not load ElioTargets.cmake")
endif()

if(NOT _find_lib_pos LESS _targets_pos
   OR NOT _find_include_pos LESS _targets_pos
   OR NOT _target_pos LESS _targets_pos)
    message(FATAL_ERROR "liburing must be restored before ElioTargets.cmake is loaded")
endif()
