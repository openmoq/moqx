# Single source of truth for the revisions moqx fetches via CPM. One pin per
# line — a bump is then a one-line diff.
#
# Shell consumers read pins through cmake, never by regexing this file:
#   cmake -DPIN=MOXYGEN_REV -P cmake/print-pin.cmake
#
# Override for local development without editing this file:
#   -DCPM_moxygen_SOURCE=/path/to/local/moxygen
#   -DCPM_catapult_SOURCE=/path/to/local/catapult
#
# The Meta stack (folly/fizz/wangle/mvfst/proxygen/picoquic) is NOT pinned here —
# moxygen owns those revisions in its build/deps/github_hashes/.

# Plain set(), not CACHE: this file must always win, so a pin bump takes effect
# on the next reconfigure of an existing build dir. A cached pin would silently
# shadow the file's value.
set(MOXYGEN_REPOSITORY "openmoq/moxygen")
set(MOXYGEN_REV "f8840755e47c02e43aa1acba05dedf3864459769")

set(CATAPULT_REPOSITORY "Quicr/catapult")
set(CATAPULT_REV "2bbf479fe2e65e425624316d335443a8c0fc0507")

# Release tags, not shas — these projects publish stable tagged releases.
set(YAMLCPP_VERSION "0.8.0")
set(REFLECTCPP_VERSION "v0.18.0")
