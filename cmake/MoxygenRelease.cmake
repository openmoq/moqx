# MoxygenRelease.cmake — map the pinned MOXYGEN_REV to the moxygen release that
# publishes its prebuilt tarballs.
#
# Function definitions only, no side effects, so the project build and a
# `cmake -P` script can both include it and resolve a pin identically.
#
# Requires cmake/dependencies.cmake first (MOXYGEN_REPOSITORY, MOXYGEN_REV).

# Empty means resolve the tag from MOXYGEN_REV; release branches pin it for a
# stable source (docs/release.md). Declared here, not at the fetch, so every
# consumer of moqx_moxygen_release_tag() resolves the same tag — the release
# workflow reads it through cmake/print-release-tag.cmake.
#
# Guarded rather than plain set(CACHE): a plain one erases a normal variable of
# the same name under CMP0126 OLD, which is what a `set(MOXYGEN_RELEASE_TAG …)`
# in cmake/dependencies.cmake is.
if(NOT DEFINED MOXYGEN_RELEASE_TAG)
  set(MOXYGEN_RELEASE_TAG ""
      CACHE STRING "moxygen release tag to fetch the prebuilt from (empty = derive from MOXYGEN_REV)")
endif()

# Return the release tag whose assets belong to MOXYGEN_REV. Two modes:
#
# Normal builds derive it from the pin: moxygen publishes every build as a
# retained snapshot-<sha12> pre-release (openmoq/scripts/publish-artifacts.sh
# in the fork), so no tag search is needed — the fetch verifies the release
# exists and that its commit matches the pin.
#
# Release builds set MOXYGEN_RELEASE_TAG to the moxygen v* release, the
# permanent artifact: snapshots are pruned after 30 days, so a release built
# on one loses reproducibility. version-release enforces this for release
# branches.
function(moqx_moxygen_release_tag OUT_VAR)
  # An explicit tag wins; the fetch verifies the release's commit against the
  # pin either way.
  if(NOT MOXYGEN_RELEASE_TAG STREQUAL "")
    set(${OUT_VAR} "${MOXYGEN_RELEASE_TAG}" PARENT_SCOPE)
    return()
  endif()
  # Publisher tags with lowercase GITHUB_SHA; a hand-edited pin need not be.
  string(TOLOWER "${MOXYGEN_REV}" _rev_lc)
  string(SUBSTRING "${_rev_lc}" 0 12 _rev12)
  set(${OUT_VAR} "snapshot-${_rev12}" PARENT_SCOPE)
endfunction()
