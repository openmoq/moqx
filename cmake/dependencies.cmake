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
set(MOXYGEN_REV "db287a51939920994c70c584a8374832d29b39c3")

# Latest published release v1.0.0, pinned to its immutable commit.
set(CATAPULT_REPOSITORY "Quicr/catapult")
set(CATAPULT_REV "cc8aeb879c261c5b972b80e5bc426fceb8568e6d")

# Release tags, not shas — these projects publish stable tagged releases.
set(YAMLCPP_VERSION "0.8.0")
set(REFLECTCPP_VERSION "v0.22.0")
