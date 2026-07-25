# MoxygenRelease.cmake — map the pinned MOXYGEN_REV to the moxygen release that
# publishes its prebuilt tarballs.
#
# Function definitions only, no side effects, so the project build and a
# `cmake -P` script can both include it and resolve a pin identically.
#
# Requires cmake/dependencies.cmake first (MOXYGEN_REPOSITORY, MOXYGEN_REV).

# Run `git ls-remote --tags` and return the tag names pointing at MOXYGEN_REV
# (annotated tags matched via their peeled ^{} line).
function(moqx_moxygen_tags_at_pin OUT_VAR)
  # Script mode has no find_package(Git) behind it, unlike the project build.
  if(NOT DEFINED GIT_EXECUTABLE OR GIT_EXECUTABLE STREQUAL "")
    find_package(Git QUIET)
  endif()
  if(NOT GIT_EXECUTABLE)
    message(FATAL_ERROR
      "moxygen: git is required to resolve the release for MOXYGEN_REV")
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" ls-remote --tags "https://github.com/${MOXYGEN_REPOSITORY}"
    OUTPUT_VARIABLE _ls_out RESULT_VARIABLE _ls_rc ERROR_VARIABLE _ls_err)
  if(NOT _ls_rc EQUAL 0)
    message(FATAL_ERROR
      "moxygen: 'git ls-remote https://github.com/${MOXYGEN_REPOSITORY}' failed: ${_ls_err}")
  endif()
  string(REGEX REPLACE "\r?\n" ";" _ls_lines "${_ls_out}")
  set(_matched "")
  foreach(_line IN LISTS _ls_lines)
    if(_line STREQUAL "")
      continue()
    endif()
    string(REGEX MATCH "^[0-9a-fA-F]+" _sha "${_line}")
    if(_sha STREQUAL "${MOXYGEN_REV}" AND _line MATCHES "refs/tags/(.+)$")
      set(_t "${CMAKE_MATCH_1}")
      string(REGEX REPLACE "\\^\\{\\}$" "" _t "${_t}")  # peel annotated-tag suffix
      list(APPEND _matched "${_t}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _matched)
  set(${OUT_VAR} "${_matched}" PARENT_SCOPE)
endfunction()

# Return the release tag whose assets belong to MOXYGEN_REV. GitHub keeps no
# commit->release index, so this asks the remote which tags point at the pin, and
# prefers an immutable v* release over a mutable snapshot-*.
function(moqx_moxygen_release_tag OUT_VAR)
  moqx_moxygen_tags_at_pin(_matched)
  set(_tag "")
  foreach(_t IN LISTS _matched)
    if(NOT _t MATCHES "^snapshot")
      set(_tag "${_t}")
      break()
    endif()
  endforeach()
  if(_tag STREQUAL "" AND _matched)
    list(GET _matched 0 _tag)
  endif()
  if(_tag STREQUAL "")
    message(FATAL_ERROR
      "moxygen: no tag on ${MOXYGEN_REPOSITORY} points at the pinned MOXYGEN_REV\n"
      "  ${MOXYGEN_REV}\n"
      "so no release publishes a prebuilt for it and there is nothing to fetch.\n"
      "Expected when the rev's only tag was a rolling snapshot-* that has since\n"
      "moved on. -DMOXYGEN_RELEASE_TAG cannot rescue it: that release's assets\n"
      "moved with the tag.\n"
      "\n"
      "Build moxygen from source instead:\n"
      "  scripts/configure.sh --moxygen from-source && scripts/build.sh\n"
      "or bump the pin in cmake/dependencies.cmake to a currently published rev.")
  endif()
  set(${OUT_VAR} "${_tag}" PARENT_SCOPE)
endfunction()
