if(NOT DEFINED ELIO_SOURCE_DIR)
    message(FATAL_ERROR "ELIO_SOURCE_DIR is required")
endif()

if(NOT DEFINED ELIO_BINARY_DIR)
    message(FATAL_ERROR "ELIO_BINARY_DIR is required")
endif()

file(READ "${ELIO_SOURCE_DIR}/CMakeLists.txt" _elio_root_cmake)

foreach(_required_text IN ITEMS
    "find_package(Threads REQUIRED)"
    "Threads::Threads"
)
    string(FIND "${_elio_root_cmake}" "${_required_text}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "Missing Threads package metadata: ${_required_text}")
    endif()
endforeach()

string(REGEX MATCH
    "target_link_libraries\\([ \t\r\n]*elio[ \t\r\n]+INTERFACE[^)]*Threads::Threads"
    _elio_threads_link "${_elio_root_cmake}")
if(NOT _elio_threads_link)
    message(FATAL_ERROR
        "Missing Threads::Threads in elio's INTERFACE link libraries")
endif()

string(REGEX MATCH
    "target_link_libraries\\([ \t\r\n]*elio[ \t\r\n]+INTERFACE[^)]*pthread"
    _elio_raw_pthread_link "${_elio_root_cmake}")
if(_elio_raw_pthread_link)
    message(FATAL_ERROR "elio must not export raw pthread in INTERFACE links")
endif()

include(CMakePackageConfigHelpers)

file(MAKE_DIRECTORY "${ELIO_BINARY_DIR}")
set(ELIO_PACKAGE_NEEDS_OPENSSL OFF)
set(ELIO_PACKAGE_NEEDS_NGHTTP2 OFF)
set(ELIO_PACKAGE_NEEDS_LIBURING OFF)
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
    "find_dependency(Threads REQUIRED)" _find_threads_pos)
string(FIND "${_elio_config}"
    "include(\${CMAKE_CURRENT_LIST_DIR}/ElioTargets.cmake)" _targets_pos)

if(_find_threads_pos EQUAL -1)
    message(FATAL_ERROR "ElioConfig.cmake does not restore Threads")
endif()

if(_targets_pos EQUAL -1)
    message(FATAL_ERROR "ElioConfig.cmake does not load ElioTargets.cmake")
endif()

if(NOT _find_threads_pos LESS _targets_pos)
    message(FATAL_ERROR
        "Threads must be restored before ElioTargets.cmake is loaded")
endif()
