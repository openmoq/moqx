# Build identifier compiled into the binary (/info, --version, log banner) and
# written to ${CMAKE_BINARY_DIR}/VERSION for packaging.
#
# Format is git describe output keeping the tag's "v" prefix, e.g. v0.2.1,
# v0.2.1-14-gabc1234, v0.2.1-14-gabc1234-dirty, or a bare sha when no v* tag is
# reachable. Resolution order:
#
#   1. -DMOQX_VERSION_STRING   explicit (CI, docker --build-arg)
#   2. git describe            any clone, including local dev builds
#   3. <source root>/VERSION   for trees with no .git (source tarballs)
#   4. v${PROJECT_VERSION}
#
# git outranks the VERSION file because that file is gitignored: a stale one
# left in a working tree would silently pin every later build.
#
# Only v-prefixed numeric tags are matched — the repo carries moving tags that
# are not versions, and an unfiltered describe would return one of those.
#
# Resolution runs at configure time, so a dev who commits without re-running
# cmake keeps the previous string until the next configure.

function(_moqx_version_from_git out_var)
  set(${out_var} "" PARENT_SCOPE)

  find_package(Git QUIET)
  if(NOT GIT_FOUND)
    return()
  endif()

  # A worktree's .git is a file, not a directory.
  if(NOT EXISTS "${PROJECT_SOURCE_DIR}/.git")
    return()
  endif()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v[0-9]*" --always
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE _described
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _rc
  )
  if(NOT _rc EQUAL 0 OR NOT _described)
    return()
  endif()

  # describe --dirty only inspects tracked files; status also reports untracked
  # ones, so a tree with new files is not mistaken for a clean commit. Ignored
  # paths (build dirs, VERSION) are excluded by default.
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" status --porcelain
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE _status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(_status)
    set(_described "${_described}-dirty")
  endif()

  set(${out_var} "${_described}" PARENT_SCOPE)
endfunction()

if(MOQX_VERSION_STRING)
  set(_moqx_version_source "explicit -DMOQX_VERSION_STRING")
else()
  _moqx_version_from_git(MOQX_VERSION_STRING)
  if(MOQX_VERSION_STRING)
    set(_moqx_version_source "git describe")
  elseif(EXISTS "${PROJECT_SOURCE_DIR}/VERSION")
    file(READ "${PROJECT_SOURCE_DIR}/VERSION" MOQX_VERSION_STRING)
    string(STRIP "${MOQX_VERSION_STRING}" MOQX_VERSION_STRING)
    set(_moqx_version_source "VERSION file")
  else()
    set(MOQX_VERSION_STRING "v${PROJECT_VERSION}")
    set(_moqx_version_source "project() fallback")
  endif()
endif()

message(STATUS "moqx version: ${MOQX_VERSION_STRING} (from ${_moqx_version_source})")

set(MOQX_VERSION_HEADER_DIR "${CMAKE_BINARY_DIR}/generated")
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/Version.h.in"
  "${MOQX_VERSION_HEADER_DIR}/moqx/Version.h"
  @ONLY
)

# Interface target so any binary can include moqx/Version.h.
add_library(moqx_version INTERFACE)
target_include_directories(moqx_version INTERFACE "${MOQX_VERSION_HEADER_DIR}")

# Shipped in the install tree: identifies an unpacked tarball without running
# the binary. Images carry OCI labels instead.
file(WRITE "${CMAKE_BINARY_DIR}/VERSION" "${MOQX_VERSION_STRING}\n")
