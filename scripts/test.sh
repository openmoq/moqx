#!/usr/bin/env bash
# test.sh — run the moqx test suite.
#
# Usage: test.sh [PROFILE] [CTEST_ARGS...]
#   PROFILE: default | san | tsan, or any preset from CMakeUserPresets.json
#   test.sh                     # all tests (default profile)
#   test.sh san                 # all tests, sanitizer build
#   test.sh default -R cache    # only tests matching 'cache'
#
# Env: MOQX_TEST_JOBS caps ctest parallelism (default: MOQX_BUILD_JOBS, else all
# cores). Lower it for sanitizer builds: instrumented binaries are slow enough
# that a full-parallel run starves the timing-sensitive integration tests.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

die() { echo "test.sh: $*" >&2; exit 1; }
. "$ROOT/scripts/lib/jobs.sh"

case "${1:-}" in
  -h|--help) awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "${BASH_SOURCE[0]}"; exit 0 ;;
esac

profile="default"
if (($#)) && [[ "$1" != -* ]]; then profile="$1"; shift; fi
build_dir="build/$profile"
[[ -f "$build_dir/CMakeCache.txt" ]] \
  || die "no configured build dir '$build_dir' — build first (scripts/configure.sh $profile --moxygen …, scripts/build.sh $profile)"

# resolve_jobs, so a typo'd count is rejected here rather than by ctest, and so
# MOQX_TEST_JOBS/MOQX_BUILD_JOBS mean the same thing they do in the sibling
# scripts. No RAM derate: running tests is not compiling them.
jobs="$(resolve_jobs "${MOQX_TEST_JOBS:-}")"
# --parallel: shell tests carry unique ports so they can run concurrently.
# Explicit job count — a bare --parallel would swallow the next argument.
ctest --test-dir "$build_dir" --output-on-failure --parallel "$jobs" "$@"
