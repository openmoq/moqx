#!/usr/bin/env bash
# changelog-section.sh — print the body of one release section of CHANGELOG.md.
#
# Usage: scripts/dev/changelog-section.sh <version> [<file>]
#
#   <version>  e.g. 1.2.3, matching the `[1.2.3] - YYYY-MM-DD` heading.
#   <file>     default: CHANGELOG.md at the repository root.
#
# - Prints the lines between that heading and the next release heading, with
#   surrounding blank lines trimmed.
# - Exits 1 when the file has no section for <version>.
# - Expects setext version headings; see CONTRIBUTING.md#changelog.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

version="${1:?usage: changelog-section.sh <version> [<file>]}"
file="${2:-$ROOT/CHANGELOG.md}"

awk -v want="[$version]" '
  { line[NR] = $0 }
  END {
    # A release heading is a line starting "[" whose next line is all "=".
    for (i = 1; i < NR; i++) {
      if (line[i + 1] !~ /^=+$/ || substr(line[i], 1, 1) != "[") continue
      if (start) { stop = i; break }
      if (index(line[i], want) == 1) start = i + 2
    }
    if (!start) exit 1
    if (!stop) stop = NR + 1
    while (start < stop && line[start] ~ /^[[:space:]]*$/) start++
    while (stop > start && line[stop - 1] ~ /^[[:space:]]*$/) stop--
    for (i = start; i < stop; i++) print line[i]
  }
' "$file"
