#!/usr/bin/env bash
# configure.sh — bind a build profile to a moxygen and configure it.
#
# Usage:
#   configure.sh [PROFILE] --mode (prebuilt|from-source)
#                [--moxygen-dir DIR] [--clean] [-j N]
#   PROFILE:             default | san | tsan, or any preset from
#                        CMakeUserPresets.json; inherit `default` so binaryDir
#                        stays build/<name>. Default: default.
#   --mode prebuilt:     download a prebuilt moxygen for the pinned rev (fast)
#   --mode from-source:  build moxygen (+ the folly/… stack) from source
#   --moxygen-dir DIR:   with from-source, build the local checkout DIR
#                        (the cross-repo moxygen+moqx dev loop; see BUILD.md)
#   --clean:             also discard this profile's from-source moxygen build
#                        (build/<PROFILE> is rebuilt from scratch either way;
#                        no effect in prebuilt mode)
#
# A preset that enables MOQX_ENABLE_SANITIZERS/MOQX_ENABLE_TSAN gets a matching
# instrumented moxygen from --mode from-source (derived from the preset's cache
# variables; override with the MOQX_MOXYGEN_PROFILE env var).
#
# Env: MOQX_BUILD_JOBS is the job count for the moxygen source build when -j is
# absent. Both override the default (cores, derated by free RAM for sanitizer
# profiles, whose coroutine-heavy TUs each peak >2 GB). See scripts/lib/jobs.sh.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SCRATCH="${MOQX_SCRATCH_PATH:-$ROOT/.scratch}"
# CPM's source clones and the extracted prebuilt installs sit side by side under
# ~/.cache/moqx. The superbuild and the moqx build share one CPM cache, so
# from-source moqx reuses the exact source its prefix was built from.
export MOQX_DEPS_CACHE="${MOQX_DEPS_CACHE:-${HOME:-$SCRATCH}/.cache/moqx}"
export CPM_SOURCE_CACHE="${CPM_SOURCE_CACHE:-$MOQX_DEPS_CACHE/cpm}"

die() { echo "configure.sh: $*" >&2; exit 1; }
usage() { awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "${BASH_SOURCE[0]}"; exit "${1:-0}"; }
. "$ROOT/scripts/lib/jobs.sh"

profile="default" mode="" moxygen_dir="" clean=0 jobs=""
if (($#)) && [[ "$1" != -* ]]; then profile="$1"; shift; fi
while (($#)); do
  case "$1" in
    --mode)        (($# >= 2)) || die "--mode needs a value (prebuilt|from-source)"; mode="$2"; shift 2 ;;
    --moxygen-dir) (($# >= 2)) || die "--moxygen-dir needs a directory"; moxygen_dir="$(cd "$2" 2>/dev/null && pwd)" || die "--moxygen-dir: '$2' not found"; shift 2 ;;
    --clean)       clean=1; shift ;;
    -j|--jobs)     (($# >= 2)) || die "$1 needs a job count"; jobs="$2"; shift 2 ;;
    -j*)           jobs="${1#-j}"; shift ;;
    --jobs=*)      jobs="${1#--jobs=}"; shift ;;
    -h|--help)     usage ;;
    *)             die "unknown option '$1' (see --help)" ;;
  esac
done
case "$mode" in
  prebuilt|from-source) ;;
  "") die "choose where moxygen comes from:
  configure.sh [PROFILE] --mode prebuilt       download the install for the pinned rev (fast)
  configure.sh [PROFILE] --mode from-source    build it from source (any rev/platform; moxygen dev)" ;;
  *)  die "unknown --mode '$mode' (prebuilt|from-source)" ;;
esac
[[ -n "$moxygen_dir" && "$mode" != from-source ]] && die "--moxygen-dir requires --mode from-source"

# -N resolves the preset's cache variables without configuring, so an unknown
# name dies here with cmake's list of presets. Those variables also say which
# moxygen is needed; MOQX_MOXYGEN_PROFILE overrides the derivation.
preset_info="$(cmake --preset "$profile" -N)"
# What the preset declares it needs...
if grep -qE 'MOQX_ENABLE_SANITIZERS(:[A-Z]+)?="?(ON|TRUE|1)"?' <<<"$preset_info"; then
  needs_profile="san"
elif grep -qE 'MOQX_ENABLE_TSAN(:[A-Z]+)?="?(ON|TRUE|1)"?' <<<"$preset_info"; then
  needs_profile="tsan"
else
  needs_profile="default"
fi
# ...versus the moxygen actually built/downloaded.
moxygen_profile="${MOQX_MOXYGEN_PROFILE:-$needs_profile}"

# Two ways to lose the instrumented moxygen a sanitizer build needs: --mode
# prebuilt, and MOQX_MOXYGEN_PROFILE downgrading the derivation. Neither is
# visible to the moqx build, so refuse both here.
if [[ "$needs_profile" != default && -z "${MOQX_ALLOW_UNINSTRUMENTED_DEPS:-}" \
      && ( "$mode" == prebuilt || "$moxygen_profile" == default ) ]]; then
  die "preset '$profile' enables sanitizers ($needs_profile) and needs an instrumented moxygen —
  use: configure.sh $profile --mode from-source   (without MOQX_MOXYGEN_PROFILE=default)
  (or set MOQX_ALLOW_UNINSTRUMENTED_DEPS=1 to knowingly link an uninstrumented moxygen)"
fi

build_dir="build/$profile"
sb="$SCRATCH/moxygen-build"; [[ "$profile" != default ]] && sb+="-$profile"
if ((clean)) && [[ -d "$sb" ]]; then
  rm -rf "$sb"
  echo "configure.sh: removed $sb"
fi

extra=("-DMOQX_MOXYGEN_PREBUILT=ON")
# One opt-in for the whole degraded combination: uninstrumented deps also imply
# the ABI skew CMakeLists refuses (Debug preset over the NDEBUG prebuilt).
[[ "$needs_profile" != default && -n "${MOQX_ALLOW_UNINSTRUMENTED_DEPS:-}" ]] \
  && extra+=("-DMOQX_ALLOW_ABI_SKEW=ON")
if [[ "$mode" == from-source ]]; then
  # Each profile gets its own superbuild dir, since a custom preset may change
  # ABI-relevant flags and sharing one cannot be assumed safe.
  cfg=(cmake -S superbuild -B "$sb" -G Ninja)
  # Passed even when default: a reused superbuild dir caches the profile, and a
  # stale san/tsan value would rebuild an instrumented moxygen under a
  # non-instrumented moqx.
  cfg+=("-DMOQX_MOXYGEN_PROFILE=$moxygen_profile")
  if [[ -n "$moxygen_dir" ]]; then
    # Local checkout: rebuild on every configure so source edits take effect.
    cfg+=("-DCPM_moxygen_SOURCE=$moxygen_dir" "-DMOQX_MOXYGEN_BUILD_ALWAYS=ON")
  else
    # Clear any cached local-checkout override from a previous configure.
    cfg+=("-UCPM_moxygen_SOURCE" "-DMOQX_MOXYGEN_BUILD_ALWAYS=OFF")
  fi
  "${cfg[@]}"
  # moxygen_profile, not needs_profile: it names the stack actually being
  # compiled here, which is what has to fit in RAM.
  sb_sanitized=""
  if [[ "$moxygen_profile" != default ]]; then sb_sanitized=1; fi
  sb_jobs="$(resolve_jobs "$jobs" "$sb_sanitized")"
  # The ExternalProject runs its own `cmake --build`, which honors the env var
  # but not the outer -j. Without it the folly stack compiles at all cores and
  # OOMs the low-RAM hosts the job count exists to protect.
  CMAKE_BUILD_PARALLEL_LEVEL="$sb_jobs" cmake --build "$sb" -j"$sb_jobs"
  echo "configure.sh: moxygen installed to $sb/moxygen-install"
  extra=("-DMOQX_MOXYGEN_PREBUILT=OFF" "-DCMAKE_PREFIX_PATH=$sb/moxygen-install")
  # Local moxygen: point moqx's find-modules at the same checkout as the libs.
  [[ -n "$moxygen_dir" ]] && extra+=("-DCPM_moxygen_SOURCE=$moxygen_dir")
fi

# Fresh configure: a build dir binds to the moxygen it first resolved, so a
# clean slate is the only way to (re)bind the choice. Prebuilt mode downloads
# its install right here.
rm -rf "$build_dir"
cmake --preset "$profile" "${extra[@]}"
# The fresh-slate wipe above only holds when the preset lands where we wiped.
[[ -f "$build_dir/CMakeCache.txt" ]] || die "preset '$profile' did not configure into $build_dir —
  the wrapper needs binaryDir \${sourceDir}/build/\${presetName}; inherit the 'default' preset."
echo "configure.sh: $build_dir configured ($mode) — compile with: scripts/build.sh $profile"
