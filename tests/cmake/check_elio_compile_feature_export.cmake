if(NOT DEFINED ELIO_SOURCE_DIR)
    message(FATAL_ERROR "ELIO_SOURCE_DIR is required")
endif()

file(READ "${ELIO_SOURCE_DIR}/CMakeLists.txt" _elio_root_cmake)

string(FIND "${_elio_root_cmake}" "add_library(elio INTERFACE)" _target_pos)
string(FIND "${_elio_root_cmake}"
    "target_compile_features(elio INTERFACE cxx_std_20)" _feature_pos)
string(FIND "${_elio_root_cmake}"
    "install(TARGETS \${ELIO_EXPORT_TARGETS} EXPORT ElioTargets)" _install_pos)

if(_target_pos EQUAL -1)
    message(FATAL_ERROR "Missing elio interface target")
endif()

if(_feature_pos EQUAL -1)
    message(FATAL_ERROR "Elio::elio must propagate cxx_std_20 to consumers")
endif()

if(_install_pos EQUAL -1)
    message(FATAL_ERROR "Missing ElioTargets export install rule")
endif()

if(NOT _target_pos LESS _feature_pos OR NOT _feature_pos LESS _install_pos)
    message(FATAL_ERROR "cxx_std_20 must be declared on elio before exporting ElioTargets")
endif()
