#!/usr/bin/env bash
# jobs.sh — resolve the compile job count. Sourced by the configure/build/test
# trilogy; not executable on its own.
#
# resolve_jobs [FLAG_VALUE] [SANITIZED]
#   FLAG_VALUE  the -j/--jobs argument, empty when not given
#   SANITIZED   non-empty when the build instruments with ASan/TSan
#
# Precedence: -j, then MOQX_BUILD_JOBS, then the default. An explicit value is
# never clamped in either direction — distcc wants far more jobs than local
# cores, a RAM-starved host wants fewer.
#
# The default is the core count, derated by free RAM for sanitizer builds only:
# those TUs carry folly's coroutines through -O2 -g plus ASan/UBSan and peak
# >2 GB each, so the core count OOMs the compiler on a RAM-bound host.
# Uninstrumented builds are roughly 4x lighter and stay at the core count.

# Callers all define die(); this is only reached if one forgets.
declare -F die >/dev/null || die() { echo "$*" >&2; exit 1; }

_cores() { nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4; }

# Free RAM in whole GB, or 0 when it cannot be read — an unknown budget must
# not silently derate the build. macOS reports total, not available: it has no
# cheap MemAvailable equivalent, and overcommitting there costs swap, not a kill.
_avail_gb() {
  if [[ -r /proc/meminfo ]]; then
    awk '/^MemAvailable:/ { print int($2 / 1048576); found = 1 } END { if (!found) print 0 }' /proc/meminfo
  elif [[ "$(uname)" == Darwin ]]; then
    local bytes
    bytes=$(sysctl -n hw.memsize 2>/dev/null) && echo $(( bytes / 1073741824 )) || echo 0
  else
    echo 0
  fi
}

resolve_jobs() {
  local jobs="${1:-${MOQX_BUILD_JOBS:-}}" sanitized="${2:-}"
  if [[ -z "$jobs" ]]; then
    jobs="$(_cores)"
    if [[ -n "$sanitized" ]]; then
      local cap=$(( $(_avail_gb) * 2 / 5 ))   # one job per 2.5 GB
      (( cap > 0 && cap < jobs )) && jobs="$cap"
    fi
  fi
  [[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die "invalid job count '$jobs' (expected a positive integer)"
  echo "$jobs"
}
