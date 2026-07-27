# Version derivation for moqx.
#
# Produces MOQX_VERSION_STRING: one canonical identifier consumed by every
# binary (admin /info, --version, the startup log banner) and embedded in
# build artifacts as a VERSION file, so a tarball or container can be
# identified without its git history.
#
# Format is git-describe output with the "v" tag prefix retained, so the
# string round-trips into `git checkout` / `gh release view`:
#
#   v0.2.1                     exact release tag
#   v0.2.1-14-gabc1234         14 commits past v0.2.1
#   v0.2.1-14-gabc1234-dirty   ...with uncommitted changes (local builds)
#   abc1234                    no reachable v* tag (shallow clone, fork)
#
# Resolution order, first hit wins:
#
#   1. -DMOQX_VERSION_STRING=...  explicit override (CI, docker --build-arg)
#   2. <source root>/VERSION      source tarballs and the docker build context
#                                 carry no .git; CI writes this file for them
#   3. git describe               any real clone, including local dev builds
#   4. v${PROJECT_VERSION}        last resort, e.g. v0.1.0
#
# Only v-prefixed numeric tags are considered. The repo also carries moving
# tags (snapshot-latest, build-*, archive/*) that are NOT versions; an
# unfiltered `git describe` happily returns "snapshot-latest" and poisons
# the version of every artifact built from main.
#
# Derivation happens at configure time. A dev who commits without re-running
# cmake keeps the previous string until the next configure; CI configures
# fresh every run, so published artifacts are always exact.

function(_moqx_version_from_git out_var)
  set(${out_var} "" PARENT_SCOPE)

  find_package(Git QUIET)
  if(NOT GIT_FOUND)
    return()
  endif()

  # A worktree's .git is a file, not a directory — EXISTS covers both.
  if(NOT EXISTS "${PROJECT_SOURCE_DIR}/.git")
    return()
  endif()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v[0-9]*" --always --dirty
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE _described
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _rc
  )
  if(_rc EQUAL 0 AND _described)
    set(${out_var} "${_described}" PARENT_SCOPE)
  endif()
endfunction()

if(MOQX_VERSION_STRING)
  set(_moqx_version_source "explicit -DMOQX_VERSION_STRING")
elseif(EXISTS "${PROJECT_SOURCE_DIR}/VERSION")
  file(READ "${PROJECT_SOURCE_DIR}/VERSION" MOQX_VERSION_STRING)
  string(STRIP "${MOQX_VERSION_STRING}" MOQX_VERSION_STRING)
  set(_moqx_version_source "VERSION file")
else()
  _moqx_version_from_git(MOQX_VERSION_STRING)
  if(MOQX_VERSION_STRING)
    set(_moqx_version_source "git describe")
  else()
    set(MOQX_VERSION_STRING "v${PROJECT_VERSION}")
    set(_moqx_version_source "project() fallback")
  endif()
endif()

message(STATUS "moqx version: ${MOQX_VERSION_STRING} (from ${_moqx_version_source})")

# Generated header — any target may link moqx_version to use it, so the
# version is not welded to one library the way a target_compile_definitions
# would be.
set(MOQX_VERSION_HEADER_DIR "${CMAKE_BINARY_DIR}/generated")
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/Version.h.in"
  "${MOQX_VERSION_HEADER_DIR}/moqx/Version.h"
  @ONLY
)

add_library(moqx_version INTERFACE)
target_include_directories(moqx_version INTERFACE "${MOQX_VERSION_HEADER_DIR}")

# Artifact-side copy: `cat VERSION` identifies an unpacked tarball or an
# image layer with no binary to run and no git history to consult.
file(WRITE "${CMAKE_BINARY_DIR}/VERSION" "${MOQX_VERSION_STRING}\n")
