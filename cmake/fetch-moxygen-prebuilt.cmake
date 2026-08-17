# fetch-moxygen-prebuilt.cmake — put the prebuilt moxygen install for the pinned
# MOXYGEN_REV in the dependency cache, with no moqx configure around it:
#
#   cmake -DOUT=<file> [-DMOQX_PLATFORM=<tag>] -P cmake/fetch-moxygen-prebuilt.cmake
#
#   OUT              file to write the install prefix to. stdout carries download
#                    progress, so the path needs a channel of its own.
#   MOQX_PLATFORM    override the auto-detected platform tag
#
# Exits non-zero when the pin has no published prebuilt for this platform, so the
# caller can pair it with the superbuild the way `scripts/configure.sh --moxygen
# prebuilt-with-fallback` does. docker/Dockerfile is why this exists standalone:
# its moxygen layer must not depend on src/, or every source change would rebuild
# the whole folly stack.
#
# cmake_minimum_required matters here — a -P script without one runs under
# old-policy defaults, and the included files are dense with if(x STREQUAL "…").
cmake_minimum_required(VERSION 3.23)
if(NOT DEFINED OUT)
  message(FATAL_ERROR "usage: cmake -DOUT=<file> -P cmake/fetch-moxygen-prebuilt.cmake")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/dependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/FetchMoxygenPrebuilt.cmake")
file(WRITE "${OUT}" "${_install_dir}")
