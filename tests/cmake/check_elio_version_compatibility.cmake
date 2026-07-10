if(NOT DEFINED ELIO_VERSION_FILE)
    message(FATAL_ERROR "ELIO_VERSION_FILE is required")
endif()

if(NOT EXISTS "${ELIO_VERSION_FILE}")
    message(FATAL_ERROR "Elio package version file not found: ${ELIO_VERSION_FILE}")
endif()

# Include the generated file once with a neutral request to discover the
# installed PACKAGE_VERSION. Subsequent checks run in function scope and do not
# leak their PACKAGE_FIND_VERSION variables into one another.
set(PACKAGE_FIND_VERSION "0")
set(PACKAGE_FIND_VERSION_MAJOR 0)
set(PACKAGE_FIND_VERSION_MINOR 0)
set(PACKAGE_FIND_VERSION_PATCH 0)
set(PACKAGE_FIND_VERSION_TWEAK 0)
set(PACKAGE_FIND_VERSION_COUNT 1)
set(PACKAGE_FIND_VERSION_RANGE FALSE)
include("${ELIO_VERSION_FILE}")
set(_elio_package_version "${PACKAGE_VERSION}")

string(REPLACE "." ";" _installed_components "${_elio_package_version}")
list(GET _installed_components 0 _installed_major)
list(GET _installed_components 1 _installed_minor)
list(GET _installed_components 2 _installed_patch)

function(check_elio_version request expected_compatible)
    string(REPLACE "." ";" _components "${request}")
    list(LENGTH _components PACKAGE_FIND_VERSION_COUNT)
    list(GET _components 0 PACKAGE_FIND_VERSION_MAJOR)

    set(PACKAGE_FIND_VERSION_MINOR 0)
    set(PACKAGE_FIND_VERSION_PATCH 0)
    set(PACKAGE_FIND_VERSION_TWEAK 0)
    if(PACKAGE_FIND_VERSION_COUNT GREATER 1)
        list(GET _components 1 PACKAGE_FIND_VERSION_MINOR)
    endif()
    if(PACKAGE_FIND_VERSION_COUNT GREATER 2)
        list(GET _components 2 PACKAGE_FIND_VERSION_PATCH)
    endif()
    if(PACKAGE_FIND_VERSION_COUNT GREATER 3)
        list(GET _components 3 PACKAGE_FIND_VERSION_TWEAK)
    endif()

    set(PACKAGE_FIND_VERSION "${request}")
    set(PACKAGE_FIND_VERSION_RANGE FALSE)
    unset(PACKAGE_VERSION_COMPATIBLE)
    unset(PACKAGE_VERSION_EXACT)
    unset(PACKAGE_VERSION_UNSUITABLE)
    include("${ELIO_VERSION_FILE}")

    if(expected_compatible AND NOT PACKAGE_VERSION_COMPATIBLE)
        message(FATAL_ERROR
            "Elio package unexpectedly rejected compatible version ${request}")
    endif()
    if(NOT expected_compatible AND PACKAGE_VERSION_COMPATIBLE)
        message(FATAL_ERROR
            "Elio package unexpectedly accepted incompatible version ${request}")
    endif()
endfunction()

check_elio_version("${_elio_package_version}" TRUE)

if(_installed_major EQUAL 0)
    # Pre-1.0 releases use the minor version as the compatibility boundary.
    check_elio_version("0.${_installed_minor}" TRUE)
    if(_installed_patch GREATER 0)
        math(EXPR _previous_patch "${_installed_patch} - 1")
        check_elio_version("0.${_installed_minor}.${_previous_patch}" TRUE)
    endif()
    if(_installed_minor GREATER 0)
        math(EXPR _previous_minor "${_installed_minor} - 1")
        check_elio_version("0.${_previous_minor}" FALSE)
    endif()
    math(EXPR _next_minor "${_installed_minor} + 1")
    check_elio_version("0.${_next_minor}" FALSE)
else()
    # Stable releases use the major version as the compatibility boundary.
    check_elio_version("${_installed_major}.0" TRUE)
    math(EXPR _previous_major "${_installed_major} - 1")
    check_elio_version("${_previous_major}.0" FALSE)
    math(EXPR _next_major "${_installed_major} + 1")
    check_elio_version("${_next_major}.0" FALSE)
endif()
