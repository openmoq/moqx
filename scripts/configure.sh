#!/usr/bin/env bash
# configure.sh — bind a build profile to a moxygen and configure it.
#
# Usage:
#   configure.sh [PROFILE] --moxygen (prebuilt-with-fallback|prebuilt|from-source)
#                [--moxygen-dir DIR] [--clean] [-j N] [-DVAR=VALUE]...
#   PROFILE:                       default | san | tsan, or any preset from
#                                  CMakeUserPresets.json; inherit `default` so
#                                  binaryDir stays build/<name>. Default: default.
#   --moxygen prebuilt-with-fallback:
#                                  the prebuilt when one is published, else the
#                                  source build
#   --moxygen prebuilt:            download the prebuilt install for the pinned
#                                  rev; no published tarball is an error
#   --moxygen from-source:         build moxygen (+ the folly/… stack) from source
#   --moxygen-dir DIR:             with from-source, build the local checkout DIR
#                                  (the cross-repo moxygen+moqx dev loop)
#                                  Which to pick: BUILD.md#how-dependencies-work
#   --clean:                       also discard this profile's from-source moxygen
#                                  build (build/<PROFILE> is rebuilt from scratch
#                                  either way; no effect unless the source build runs)
#   -DVAR=VALUE:                   forwarded to the moqx configure. The variables
#                                  this script owns are refused — MOQX_MOXYGEN_PREBUILT,
#                                  CMAKE_PREFIX_PATH, CPM_moxygen_SOURCE,
#                                  MOQX_MOXYGEN_PROFILE and MOQX_ALLOW_ABI_SKEW are
#                                  what --moxygen, --moxygen-dir and the env vars
#                                  below decide. For anything else, drive cmake
#                                  directly (see BUILD.md).
#
# A build that enables MOQX_ENABLE_SANITIZERS/MOQX_ENABLE_TSAN gets a matching
# instrumented moxygen from --moxygen from-source (derived from the preset's cache
# variables and any -D for those two; override with the MOQX_MOXYGEN_PROFILE env
# var). Neither prebuilt nor prebuilt-with-fallback can deliver one, so both refuse
# a sanitizer build unless MOQX_ALLOW_UNINSTRUMENTED_DEPS=1 — and the fallback then
# builds the uninstrumented stack too, so losing the prebuilt cannot silently turn a
# short lane into a long instrumented one.
#
# Env: MOQX_BUILD_JOBS is the job count for the moxygen source build when -j is
# absent. Both override the default (cores, derated by free RAM for sanitizer
# profiles, whose coroutine-heavy TUs each peak >2 GB). See scripts/lib/jobs.sh.
# MOQX_MOXYGEN_FALLBACK=off reduces prebuilt-with-fallback to plain prebuilt — the
# lever to pull when a bad pin is making every CI lane compile folly.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SCRATCH="${MOQX_SCRATCH_PATH:-$ROOT/.scratch}"
# One CPM source cache for the superbuild and the moqx build, so from-source
# moqx reuses the very moxygen source the prefix was built from (its
# find-modules must match) instead of re-fetching into the build dir's _deps.
export CPM_SOURCE_CACHE="${CPM_SOURCE_CACHE:-${HOME:-$SCRATCH}/.cache/moqx/cpm}"

die() { echo "configure.sh: $*" >&2; exit 1; }
usage() { awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "${BASH_SOURCE[0]}"; exit "${1:-0}"; }
# CMake's truthy spelling, matched case-insensitively without ${x^^} — this
# script also runs under macOS's bash 3.2.
truthy() { case "$1" in [Oo][Nn]|[Tt][Rr][Uu][Ee]|1|[Yy][Ee][Ss]) return 0 ;; *) return 1 ;; esac; }
. "$ROOT/scripts/lib/jobs.sh"

profile="default" moxygen="" moxygen_dir="" clean=0 jobs="" passthru=()
if (($#)) && [[ "$1" != -* ]]; then profile="$1"; shift; fi
while (($#)); do
  case "$1" in
    --moxygen)     (($# >= 2)) || die "--moxygen needs a value (prebuilt|prebuilt-with-fallback|from-source)"; moxygen="$2"; shift 2 ;;
    --moxygen-dir) (($# >= 2)) || die "--moxygen-dir needs a directory"; moxygen_dir="$(cd "$2" 2>/dev/null && pwd)" || die "--moxygen-dir: '$2' not found"; shift 2 ;;
    --clean)       clean=1; shift ;;
    -j|--jobs)     (($# >= 2)) || die "$1 needs a job count"; jobs="$2"; shift 2 ;;
    -j*)           jobs="${1#-j}"; shift ;;
    --jobs=*)      jobs="${1#--jobs=}"; shift ;;
    -D)            (($# >= 2)) || die "-D needs VAR=VALUE"; passthru+=("-D$2"); shift 2 ;;
    -D*)           passthru+=("$1"); shift ;;
    -h|--help)     usage ;;
    *)             die "unknown option '$1' (see --help)" ;;
  esac
done
case "$moxygen" in
  prebuilt|prebuilt-with-fallback|from-source) ;;
  "") die "choose where moxygen comes from:
  configure.sh [PROFILE] --moxygen prebuilt-with-fallback   download it, or build it when none is published — start here
  configure.sh [PROFILE] --moxygen prebuilt                 download only; error when none is published
  configure.sh [PROFILE] --moxygen from-source              build it from source (any rev/platform; moxygen dev)" ;;
  *)  die "unknown --moxygen '$moxygen' (prebuilt-with-fallback|prebuilt|from-source)" ;;
esac
[[ -n "$moxygen_dir" && "$moxygen" != from-source ]] && die "--moxygen-dir requires --moxygen from-source"

# Refusing beats last-wins: a stray -DMOQX_MOXYGEN_PREBUILT=ON would defeat the
# from-source prefix and pull an uninstrumented moxygen under a sanitizer build,
# which is the case the interlock below exists to prevent.
for arg in ${passthru[@]+"${passthru[@]}"}; do
  case "$arg" in
    -DMOQX_MOXYGEN_PREBUILT[:=]*) die "-DMOQX_MOXYGEN_PREBUILT is what --moxygen selects" ;;
    -DCMAKE_PREFIX_PATH[:=]*)     die "-DCMAKE_PREFIX_PATH is set by --moxygen/--moxygen-dir; to point at your own prefix drive cmake directly (see BUILD.md)" ;;
    -DCPM_moxygen_SOURCE[:=]*)    die "-DCPM_moxygen_SOURCE is what --moxygen-dir sets" ;;
    -DMOQX_MOXYGEN_PROFILE[:=]*)  die "MOQX_MOXYGEN_PROFILE is an env var here — it goes to the superbuild, not to moqx" ;;
    -DMOQX_ALLOW_ABI_SKEW[:=]*)   die "set MOQX_ALLOW_UNINSTRUMENTED_DEPS=1 instead; it covers the whole degraded combination" ;;
  esac
done

# -N resolves the preset's cache variables without configuring, so an unknown
# name dies here with cmake's list of presets. Those variables also say which
# moxygen is needed; MOQX_MOXYGEN_PROFILE overrides the derivation.
preset_info="$(cmake --preset "$profile" -N)"
# What the preset declares it needs...
# The *_src labels name whatever settled each flag, so the errors below blame the
# thing the reader has to change.
san=0 tsan=0 san_src="preset '$profile'" tsan_src="preset '$profile'"
if grep -qE 'MOQX_ENABLE_SANITIZERS(:[A-Z]+)?="?(ON|TRUE|1)"?' <<<"$preset_info"; then san=1; fi
if grep -qE 'MOQX_ENABLE_TSAN(:[A-Z]+)?="?(ON|TRUE|1)"?' <<<"$preset_info"; then tsan=1; fi
# ...and what -D asks for on top, last value winning as it does at the cmake
# line. Read here too, or a sanitizer turned on this way walks straight past the
# interlock below and links the uninstrumented moxygen.
for arg in ${passthru[@]+"${passthru[@]}"}; do
  case "$arg" in
    -DMOQX_ENABLE_SANITIZERS[:=]*) if truthy "${arg#*=}"; then san=1; else san=0; fi
                                   san_src="$arg" ;;
    -DMOQX_ENABLE_TSAN[:=]*)       if truthy "${arg#*=}"; then tsan=1; else tsan=0; fi
                                   tsan_src="$arg" ;;
  esac
done
if ((san)); then
  needs_profile="san" needs_src="$san_src"
elif ((tsan)); then
  needs_profile="tsan" needs_src="$tsan_src"
else
  # Nothing on: whichever -D turned one off is the reason, else the preset.
  needs_profile="default" needs_src="$san_src"
  if [[ "$needs_src" == preset\'* && "$tsan_src" != preset\'* ]]; then
    needs_src="$tsan_src"
  fi
fi
# ...versus the moxygen actually built/downloaded. A fallback build has to match
# the prebuilt it stands in for, or losing the prebuilt would quietly change what
# the lane tests.
if [[ "$moxygen" == prebuilt-with-fallback ]]; then
  moxygen_profile="${MOQX_MOXYGEN_PROFILE:-default}"
else
  moxygen_profile="${MOQX_MOXYGEN_PROFILE:-$needs_profile}"
fi

# A sanitizer build loses its instrumented moxygen to a --moxygen value that cannot
# produce one, or to MOQX_MOXYGEN_PROFILE naming a different one. Neither is visible
# to the moqx build, and ASan deps under a TSan moqx link two clashing runtimes.
if [[ "$needs_profile" != default && -z "${MOQX_ALLOW_UNINSTRUMENTED_DEPS:-}" \
      && ( "$moxygen" != from-source || "$moxygen_profile" != "$needs_profile" ) ]]; then
  if [[ "$moxygen" != from-source ]]; then
    why="--moxygen $moxygen can only deliver the uninstrumented prebuilt"
  else
    why="MOQX_MOXYGEN_PROFILE=$moxygen_profile builds a different one"
  fi
  die "$needs_src enables sanitizers ($needs_profile) and needs a moxygen instrumented to match,
  but $why —
  use: configure.sh $profile --moxygen from-source   with MOQX_MOXYGEN_PROFILE unset or =$needs_profile
  (or set MOQX_ALLOW_UNINSTRUMENTED_DEPS=1 to knowingly link an uninstrumented moxygen)"
fi
# The mirror skew, which no opt-in covers: an instrumented moxygen under a moqx
# carrying no sanitizer flags leaves the __asan_/__tsan_ interceptors undefined at
# link, and the superbuild builds those profiles Debug, which skews kIsDebug too.
if [[ "$needs_profile" == default && "$moxygen_profile" != default ]]; then
  die "MOQX_MOXYGEN_PROFILE=$moxygen_profile asks for a $moxygen_profile-instrumented moxygen, but
  $needs_src enables no sanitizers, so moqx itself would carry none —
  use: configure.sh san|tsan --moxygen from-source   (or unset MOQX_MOXYGEN_PROFILE)"
fi

build_dir="build/$profile"
sb="$SCRATCH/moxygen-build"; [[ "$profile" != default ]] && sb+="-$profile"
if ((clean)) && [[ -d "$sb" ]]; then
  rm -rf "$sb"
  echo "configure.sh: removed $sb"
fi

# One opt-in for the whole degraded combination: uninstrumented deps also imply
# the ABI skew CMakeLists refuses (Debug preset over the NDEBUG prebuilt).
common=()
[[ "$needs_profile" != default && -n "${MOQX_ALLOW_UNINSTRUMENTED_DEPS:-}" ]] \
  && common+=("-DMOQX_ALLOW_ABI_SKEW=ON")

# Fresh configure: a build dir binds to the moxygen it first resolved, so a
# clean slate is the only way to (re)bind the choice.
configure_moqx() {
  rm -rf "$build_dir"
  cmake --preset "$profile" "$@" \
    ${common[@]+"${common[@]}"} ${passthru[@]+"${passthru[@]}"}
}

# Compile the moxygen prefix, then configure moqx against it.
configure_from_source() {
  # Each profile gets its own superbuild dir, since a custom preset may change
  # ABI-relevant flags and sharing one cannot be assumed safe.
  local cfg=(cmake -S superbuild -B "$sb" -G Ninja)
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
  local sb_sanitized=""
  if [[ "$moxygen_profile" != default ]]; then sb_sanitized=1; fi
  local sb_jobs; sb_jobs="$(resolve_jobs "$jobs" "$sb_sanitized")"
  # The ExternalProject runs its own `cmake --build`, which honors the env var
  # but not the outer -j. Without it the folly stack compiles at all cores and
  # OOMs the low-RAM hosts the job count exists to protect.
  CMAKE_BUILD_PARALLEL_LEVEL="$sb_jobs" cmake --build "$sb" -j"$sb_jobs"
  echo "configure.sh: moxygen installed to $sb/moxygen-install"

  local from_source=("-DMOQX_MOXYGEN_PREBUILT=OFF" "-DCMAKE_PREFIX_PATH=$sb/moxygen-install")
  # Local moxygen: point moqx's find-modules at the same checkout as the libs.
  [[ -n "$moxygen_dir" ]] && from_source+=("-DCPM_moxygen_SOURCE=$moxygen_dir")
  configure_moqx "${from_source[@]}"
}

# Is a prebuilt published for the pin on this platform? Populates the dependency
# cache when it is, so the configure that follows needs no network. Forwarding the
# probe's own knobs keeps it answering for the platform moqx goes on to ask for.
fetch_prebuilt() {
  local args=() a rc=0 out
  for a in ${passthru[@]+"${passthru[@]}"}; do
    case "$a" in -DMOQX_PLATFORM[:=]*|-DMOXYGEN_RELEASE_TAG[:=]*) args+=("$a") ;; esac
  done
  out="$(mktemp)"
  cmake -DOUT="$out" ${args[@]+"${args[@]}"} -P cmake/fetch-moxygen-prebuilt.cmake || rc=$?
  rm -f "$out"
  return "$rc"
}

resolved="$moxygen"
[[ "$moxygen" == prebuilt-with-fallback && "${MOQX_MOXYGEN_FALLBACK:-}" == off ]] && resolved="prebuilt"
case "$resolved" in
  from-source)
    configure_from_source
    ;;
  prebuilt)
    configure_moqx -DMOQX_MOXYGEN_PREBUILT=ON
    ;;
  prebuilt-with-fallback)
    # Ask the fetcher directly rather than read a failed moqx configure as "no
    # prebuilt": a broken CMakeLists or an unrelated CPM fetch would otherwise buy
    # a folly build that fails at the same place half an hour later.
    if fetch_prebuilt; then
      resolved="prebuilt"
      configure_moqx -DMOQX_MOXYGEN_PREBUILT=ON
    else
      resolved="from-source"
      echo "configure.sh: no moxygen prebuilt for the pin (see above) — building it from source, which is slow" >&2
      [[ -n "${GITHUB_ACTIONS:-}" ]] \
        && echo "::warning title=moxygen prebuilt unavailable::configure.sh fell back to the from-source superbuild"
      configure_from_source
    fi
    ;;
esac

# The fresh-slate wipe above only holds when the preset lands where we wiped.
[[ -f "$build_dir/CMakeCache.txt" ]] || die "preset '$profile' did not configure into $build_dir —
  the wrapper needs binaryDir \${sourceDir}/build/\${presetName}; inherit the 'default' preset."
if [[ "$resolved" == "$moxygen" ]]; then
  echo "configure.sh: $build_dir configured ($resolved) — compile with: scripts/build.sh $profile"
else
  echo "configure.sh: $build_dir configured ($moxygen -> $resolved) — compile with: scripts/build.sh $profile"
fi
