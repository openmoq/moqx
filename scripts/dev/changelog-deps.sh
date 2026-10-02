#!/usr/bin/env bash
# changelog-deps.sh — print the `## Dependencies` block for a CHANGELOG.md release
# section: the pins in cmake/dependencies.cmake that changed since the previous release.
#
# Usage: scripts/dev/changelog-deps.sh [<previous-tag>]
#
#   <previous-tag>  the release to compare against. Default: the latest v* tag
#                   reachable from HEAD.
#
# - Run it on the commit being released; see docs/release.md#changelog.
# - Prints nothing when no pin changed; the release then gets no Dependencies section.
# - A pin absent at <previous-tag> counts as changed.
# - moxygen is named by its v* tag when MOXYGEN_REV carries one, else by short sha.
#   Resolving the tag needs network access to github.com.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

prev="${1:-$(git describe --tags --abbrev=0 --match 'v*' HEAD)}"
if ! git cat-file -e "$prev:cmake/dependencies.cmake" 2>/dev/null; then
  echo "error: $prev has no cmake/dependencies.cmake to compare against" >&2
  exit 1
fi

# print-pin.cmake includes dependencies.cmake from its own directory, so the
# previous tag's pins are read from a copy of both files.
prev_dir=$(mktemp -d)
trap 'rm -rf "$prev_dir"' EXIT
git show "$prev:cmake/dependencies.cmake" > "$prev_dir/dependencies.cmake"
cp cmake/print-pin.cmake "$prev_dir/"

pin() { cmake -DPIN="$1" -P cmake/print-pin.cmake; }
prev_pin() { cmake -DPIN="$1" -P "$prev_dir/print-pin.cmake" 2>/dev/null || true; }
changed() { [[ "$(pin "$1")" != "$(prev_pin "$1")" ]]; }

lines=()

if changed MOXYGEN_REV; then
  repo=$(pin MOXYGEN_REPOSITORY)
  rev=$(pin MOXYGEN_REV)
  # Annotated tags list twice (tag object, then ^{} peeled to the commit); match either.
  tag=$(git ls-remote --tags "https://github.com/$repo" |
    awk -v sha="$rev" '$1 == sha { sub("refs/tags/", "", $2); sub(/\^\{\}$/, "", $2); print $2 }' |
    grep -E '^v[0-9]' | sort -uV | tail -n1 || true)
  if [[ -n "$tag" ]]; then
    lines+=("- moxygen [$tag](https://github.com/$repo/releases/tag/$tag)")
  else
    lines+=("- moxygen [${rev:0:7}](https://github.com/$repo/commit/$rev)")
  fi
fi
if changed CATAPULT_REV; then
  repo=$(pin CATAPULT_REPOSITORY)
  rev=$(pin CATAPULT_REV)
  lines+=("- catapult [${rev:0:7}](https://github.com/$repo/commit/$rev)")
fi
if changed YAMLCPP_VERSION; then
  v=$(pin YAMLCPP_VERSION)
  lines+=("- yaml-cpp [$v](https://github.com/jbeder/yaml-cpp/releases/tag/$v)")
fi
if changed REFLECTCPP_VERSION; then
  v=$(pin REFLECTCPP_VERSION)
  lines+=("- reflect-cpp [$v](https://github.com/getml/reflect-cpp/releases/tag/$v)")
fi

((${#lines[@]})) || exit 0
echo "## Dependencies"
echo
printf '%s\n' "${lines[@]}"
