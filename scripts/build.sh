#!/usr/bin/env bash
# build.sh — compile moqx
#
# Usage: build.sh [PROFILE] [CMAKE_BUILD_ARGS...]
#   PROFILE:             default | san | tsan, or any preset from
#                        CMakeUserPresets.json; the literals `setup`/`test`
#                        are reserved. Default: default.
#   -j N | --jobs N:     compile parallelism, claimed ahead of the passthrough
#   CMAKE_BUILD_ARGS:    everything after PROFILE goes to `cmake --build`,
#                        e.g. build.sh default --target moqx-issuer
#
# Run scripts/configure.sh [PROFILE] once per profile first.
#
# Env: MOQX_BUILD_JOBS is the job count when -j is absent. Both override the
# default (cores, derated by free RAM for sanitizer profiles) — set either well
# above the core count for distcc. See scripts/lib/jobs.sh.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

die() { echo "build.sh: $*" >&2; exit 1; }
. "$ROOT/scripts/lib/jobs.sh"

# Transitional: `setup` and `test` were subcommands of the old build.sh.
case "${1:-}" in
  setup|configure) die "configuring is done by scripts/configure.sh [PROFILE] --moxygen … (see its --help)" ;;
  test)            die "tests are run by scripts/test.sh [PROFILE] [CTEST_ARGS...]" ;;
  -h|--help)       awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "${BASH_SOURCE[0]}"; exit 0 ;;
esac

profile="default"
if (($#)) && [[ "$1" != -* ]]; then profile="$1"; shift; fi

# -j is claimed here rather than left to the passthrough, so the resolved count
# is the only one on the cmake line.
jobs="" passthrough=()
while (($#)); do
  case "$1" in
    -j|--jobs) (($# >= 2)) || die "$1 needs a job count"; jobs="$2"; shift 2 ;;
    -j*)       jobs="${1#-j}"; shift ;;
    --jobs=*)  jobs="${1#--jobs=}"; shift ;;
    *)         passthrough+=("$1"); shift ;;
  esac
done

build_dir="build/$profile"
[[ -f "$build_dir/CMakeCache.txt" ]] \
  || die "no configured build dir '$build_dir' — run first: scripts/configure.sh $profile --moxygen … (see its --help)"

# The configured cache is the only thing that knows whether this build is
# instrumented; the profile name alone does not (custom presets, -D overrides).
sanitized=""
if grep -qE '^MOQX_ENABLE_(SANITIZERS|TSAN):BOOL=(ON|TRUE|1)$' "$build_dir/CMakeCache.txt"; then
  sanitized=1
fi
jobs="$(resolve_jobs "$jobs" "$sanitized")"

set -x
cmake --build "$build_dir" -j"$jobs" ${passthrough[@]+"${passthrough[@]}"}
