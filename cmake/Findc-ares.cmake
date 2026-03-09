# Findc-ares.cmake
# Wraps system c-ares for platforms where libc-ares-dev does not install
# cmake config files (e.g. Ubuntu 22.04 with generic cmake >= 3.25 binary).

if(TARGET c-ares::cares)
  return()
endif()

find_library(c-ares_LIBRARY NAMES cares)
find_path(c-ares_INCLUDE_DIR NAMES ares.h)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(c-ares
  REQUIRED_VARS c-ares_LIBRARY c-ares_INCLUDE_DIR
)

if(c-ares_FOUND AND NOT TARGET c-ares::cares)
  add_library(c-ares::cares UNKNOWN IMPORTED)
  set_target_properties(c-ares::cares PROPERTIES
    IMPORTED_LOCATION "${c-ares_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${c-ares_INCLUDE_DIR}"
  )
endif()
