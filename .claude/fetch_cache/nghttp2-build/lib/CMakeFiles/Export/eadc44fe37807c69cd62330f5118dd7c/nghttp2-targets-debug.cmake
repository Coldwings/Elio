#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "nghttp2::nghttp2_static" for configuration "Debug"
set_property(TARGET nghttp2::nghttp2_static APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(nghttp2::nghttp2_static PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libnghttp2.a"
  )

list(APPEND _cmake_import_check_targets nghttp2::nghttp2_static )
list(APPEND _cmake_import_check_files_for_nghttp2::nghttp2_static "${_IMPORT_PREFIX}/lib/libnghttp2.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
