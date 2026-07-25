# print-release-tag.cmake — print the moxygen release tag the pinned MOXYGEN_REV
# resolves to:
#
#   cmake -P cmake/print-release-tag.cmake
#
# Hits the network (git ls-remote); no tag at the pin exits non-zero.
# message() writes to stderr in script mode; -E echo is the stdout channel.
include("${CMAKE_CURRENT_LIST_DIR}/dependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/MoxygenRelease.cmake")
moqx_moxygen_release_tag(_tag)
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${_tag}")
