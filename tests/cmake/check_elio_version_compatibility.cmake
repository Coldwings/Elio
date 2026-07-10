if(NOT DEFINED ELIO_VERSION_FILE)
    message(FATAL_ERROR "ELIO_VERSION_FILE is required")
endif()

if(NOT EXISTS "${ELIO_VERSION_FILE}")
    message(FATAL_ERROR "Elio package version file not found: ${ELIO_VERSION_FILE}")
endif()

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

# Elio 0.5.2 accepts patch-compatible requests in 0.5, but pre-1.0 minor
# releases are separate compatibility lines under Semantic Versioning.
check_elio_version("0.5" TRUE)
check_elio_version("0.5.0" TRUE)
check_elio_version("0.5.2" TRUE)
check_elio_version("0.4" FALSE)
check_elio_version("0.6" FALSE)
