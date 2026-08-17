#!/usr/bin/env bash
# lint.sh — run clang-tidy over moqx's own translation units.
#
# Usage: lint.sh [BUILD_DIR]   (default: build/default)
#
# The positional regex matches each compile_commands.json entry's source path.
# Without it, the CPM-fetched dependencies — two thirds of the entries — are
# analysed too.
#
# .clang-tidy does not load — it carries a key clang-tidy 16 removed — so this
# runs the default check set, not the one configured there.
# See https://github.com/openmoq/moqx/issues/518.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BUILD_DIR=${1:-build/default}

# run-clang-tidy hands the positional straight to Python's re, so the checkout
# path has to be escaped: a '+' or '(' in it fails to compile, a '.' matches wide.
ROOT_RE=$(python3 -c 'import re, sys; print(re.escape(sys.argv[1]))' "$ROOT")

run-clang-tidy -p "${BUILD_DIR}" "^${ROOT_RE}/(src|test|benchmark)/"
