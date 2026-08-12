#!/usr/bin/env bash
# lint.sh — run clang-tidy over moqx's own translation units.
#
# Usage: lint.sh [--mode=full|without-static-analyzer] [--changed[=BASE_REF]]
#                [BUILD_DIR]   (default: build/default)
#
# --mode=full: also run clang-analyzer-* (the Clang Static Analyzer), where the
# config enables it — src/, not test/ (see test/.clang-tidy). Its symbolic
# execution lands unevenly: it barely moves a small TU and more than doubles
# the heaviest. Default is --mode=without-static-analyzer, which turns it off
# everywhere for a faster local loop; CI gates on --mode=full.
#
# --changed[=BASE_REF]: only lint TUs affected by the diff against BASE_REF
# (default origin/main) — a changed .cpp directly, or a changed .h resolved
# to every TU that transitively includes it (find-affected-tus.py).
#
# Falls back to a full sweep when a changed file can move findings in TUs the
# diff doesn't name: the build files that decide what a TU compiles to, and
# any .clang-tidy. Docs, workflows and scripts cannot, so they lint nothing.
#
# The positional regex matches each compile_commands.json entry's source path.
# Without it, the CPM-fetched dependencies — two thirds of the entries — are
# analysed too.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

MODE=""
CHANGED_BASE=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode=*) MODE="${1#--mode=}"; shift ;;
    --changed) CHANGED_BASE="origin/main"; shift ;;
    --changed=*) CHANGED_BASE="${1#--changed=}"; shift ;;
    *) break ;;
  esac
done
if [[ -n "${MODE}" && "${MODE}" != "full" && "${MODE}" != "without-static-analyzer" ]]; then
  echo "lint.sh: --mode must be 'full' or 'without-static-analyzer' (got '${MODE}')" >&2
  exit 1
fi
BUILD_DIR=${1:-build/default}

REQUIRED_CT_VERSION=19

# Debian's clang-tidy package symlinks the bare names; apt.llvm.org's packages
# (what CI installs) ship only the versioned ones. Resolve explicitly either way.
CT_BIN=${CT_BIN:-$(command -v clang-tidy-${REQUIRED_CT_VERSION} || command -v clang-tidy)}
RUN_CT_BIN=${RUN_CT_BIN:-$(command -v run-clang-tidy-${REQUIRED_CT_VERSION} || command -v run-clang-tidy)}

# run-clang-tidy hands the positional straight to Python's re, so the checkout
# path has to be escaped: a '+' or '(' in it fails to compile, a '.' matches wide.
ROOT_RE=$(python3 -c 'import re, sys; print(re.escape(sys.argv[1]))' "$ROOT")

OUT=$(mktemp)
CHANGED_LIST=$(mktemp)
AFFECTED_TUS=$(mktemp)
trap 'rm -f "$OUT" "$CHANGED_LIST" "$AFFECTED_TUS"' EXIT

TU_SCOPE_RE="^${ROOT_RE}/(src|test|benchmark)/"

if [[ -n "${CHANGED_BASE}" ]]; then
  git diff --name-only "${CHANGED_BASE}...HEAD" > "${CHANGED_LIST}"

  # Only a file that can move findings in TUs the diff doesn't name widens the
  # scope: the build files deciding what each TU compiles to (flags, standard,
  # definitions, the dependency set) and the check config itself. A docs,
  # workflow or script change reaches no TU.
  FALLBACK_TO_FULL=0
  while IFS= read -r f; do
    [[ -z "$f" ]] && continue
    case "$f" in
      CMakeLists.txt|cmake/*|*/CMakeLists.txt|*.cmake|CMakePresets.json)
        FALLBACK_TO_FULL=1
        ;;
    esac
    [[ "$(basename "$f")" == ".clang-tidy" ]] && FALLBACK_TO_FULL=1
  done < "${CHANGED_LIST}"

  if [[ "${FALLBACK_TO_FULL}" -eq 1 ]]; then
    echo "lint.sh: --changed fell back to a full sweep (a build file or a" \
      ".clang-tidy changed; either can move findings in TUs the diff doesn't" \
      "name)" >&2
  else
    grep -E '\.(cpp|h)$' "${CHANGED_LIST}" | \
      python3 "${ROOT}/scripts/dev/find-affected-tus.py" "${ROOT}" > "${AFFECTED_TUS}" || true
    if [[ ! -s "${AFFECTED_TUS}" ]]; then
      echo "lint.sh: --changed found no affected TUs; nothing to lint" >&2
      exit 0
    fi
    TU_SCOPE_RE=$(python3 -c '
import re, sys
root = sys.argv[1]
with open(sys.argv[2]) as f:
    files = [l.strip() for l in f if l.strip()]
print("^(" + "|".join(re.escape(root + "/" + f) for f in files) + ")$")
' "${ROOT}" "${AFFECTED_TUS}")
  fi
fi

# clang-analyzer-* diagnostics ignore HeaderFilterRegex/--exclude-header-filter — verified
# against clang-tidy 19.1.7, where a prebuilt dependency's -isystem header still reports.
# So run-clang-tidy's own exit code isn't trustworthy for gating; grep the diagnostics
# for ones actually inside this checkout and decide pass/fail from those alone.
CHECKS_ARG=()
if [[ "${MODE}" != "full" ]]; then
  CHECKS_ARG=(-checks='-clang-analyzer-*')
fi

set +e
"${RUN_CT_BIN}" -clang-tidy-binary="${CT_BIN}" "${CHECKS_ARG[@]}" -p "${BUILD_DIR}" \
  "${TU_SCOPE_RE}" 2>&1 | tee "$OUT"
set -e

if grep -qE "^${ROOT_RE}/.*: error: " "$OUT"; then
  echo "lint.sh: clang-tidy reported errors in this checkout's own files:" >&2
  grep -E "^${ROOT_RE}/.*: error: " "$OUT" >&2
  exit 1
fi
