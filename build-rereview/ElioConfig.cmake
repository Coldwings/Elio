
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was ElioConfig.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################

include(CMakeFindDependencyMacro)

find_dependency(fmt REQUIRED)
find_dependency(Threads REQUIRED)

if(OFF)
    find_dependency(OpenSSL REQUIRED)
endif()

if(OFF)
    if(NOT TARGET nghttp2::nghttp2_static)
        set(_ELIO_NGHTTP2_TARGETS_FILE
            "${CMAKE_CURRENT_LIST_DIR}/../nghttp2/nghttp2-targets.cmake")
        if(EXISTS "${_ELIO_NGHTTP2_TARGETS_FILE}")
            include("${_ELIO_NGHTTP2_TARGETS_FILE}")
        else()
            find_library(ELIO_NGHTTP2_LIBRARY NAMES nghttp2)
            find_path(ELIO_NGHTTP2_INCLUDE_DIR NAMES nghttp2/nghttp2.h)
            if(NOT ELIO_NGHTTP2_LIBRARY OR NOT ELIO_NGHTTP2_INCLUDE_DIR)
                set(Elio_FOUND FALSE)
                set(Elio_NOT_FOUND_MESSAGE
                    "Elio HTTP/2 support requires nghttp2, but no bundled nghttp2 target or system nghttp2 library/header was found.")
                return()
            endif()
            add_library(nghttp2::nghttp2_static UNKNOWN IMPORTED)
            set_target_properties(nghttp2::nghttp2_static PROPERTIES
                IMPORTED_LOCATION "${ELIO_NGHTTP2_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${ELIO_NGHTTP2_INCLUDE_DIR}")
        endif()
        unset(_ELIO_NGHTTP2_TARGETS_FILE)
    endif()
endif()

if(OFF)
    if(NOT TARGET liburing::liburing)
        find_library(ELIO_LIBURING_LIBRARY NAMES uring)
        find_path(ELIO_LIBURING_INCLUDE_DIR NAMES liburing.h)
        if(NOT ELIO_LIBURING_LIBRARY OR NOT ELIO_LIBURING_INCLUDE_DIR)
            set(Elio_FOUND FALSE)
            set(Elio_NOT_FOUND_MESSAGE
                "Elio io_uring support requires liburing, but no system liburing library/header was found.")
            return()
        endif()
        add_library(liburing::liburing UNKNOWN IMPORTED)
        set_target_properties(liburing::liburing PROPERTIES
            IMPORTED_LOCATION "${ELIO_LIBURING_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ELIO_LIBURING_INCLUDE_DIR}")
    endif()
endif()

if(OFF)
    if(NOT TARGET rdmacm::rdmacm)
        find_library(ELIO_RDMACM_LIBRARY NAMES rdmacm)
        find_path(ELIO_RDMACM_INCLUDE_DIR NAMES rdma/rdma_cma.h)
        if(NOT ELIO_RDMACM_LIBRARY OR NOT ELIO_RDMACM_INCLUDE_DIR)
            set(Elio_FOUND FALSE)
            set(Elio_NOT_FOUND_MESSAGE
                "Elio RDMA CM support requires librdmacm, but no system librdmacm library/header was found.")
            return()
        endif()
        add_library(rdmacm::rdmacm UNKNOWN IMPORTED)
        set_target_properties(rdmacm::rdmacm PROPERTIES
            IMPORTED_LOCATION "${ELIO_RDMACM_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ELIO_RDMACM_INCLUDE_DIR}")
    endif()
endif()

if(OFF)
    if(NOT TARGET ibverbs::ibverbs)
        find_library(ELIO_IBVERBS_LIBRARY NAMES ibverbs)
        find_path(ELIO_IBVERBS_INCLUDE_DIR NAMES infiniband/verbs.h)
        if(NOT ELIO_IBVERBS_LIBRARY OR NOT ELIO_IBVERBS_INCLUDE_DIR)
            set(Elio_FOUND FALSE)
            set(Elio_NOT_FOUND_MESSAGE
                "Elio RDMA ibverbs support requires libibverbs, but no system libibverbs library/header was found.")
            return()
        endif()
        add_library(ibverbs::ibverbs UNKNOWN IMPORTED)
        set_target_properties(ibverbs::ibverbs PROPERTIES
            IMPORTED_LOCATION "${ELIO_IBVERBS_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ELIO_IBVERBS_INCLUDE_DIR}")
    endif()
endif()

if(OFF)
    find_dependency(CUDAToolkit REQUIRED)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/ElioTargets.cmake)
