#!/usr/bin/env bash
# bisect.sh — find the commit that changed moqx's behaviour, across the three
# repos one moqx build spans: moqx, the moxygen it pins (MOXYGEN_REV in
# cmake/dependencies.cmake), and the folly/fizz/wangle/mvfst/proxygen/picoquic
# revisions moxygen pins (build/deps/github_hashes/**/*-rev.txt).
#
# WHAT is bisected for is a pluggable predicate — a throughput regression is one
# use, a crash, hang, test failure or build break are equally good ones.
#
# Usage:
#   bisect.sh phase1  --good REV --bad REV        (--test CMD | --test-perf) [options]
#   bisect.sh phase2  --good REV --bad REV        (--test CMD | --test-perf) [options]
#   bisect.sh phase3  --moxygen-good SHA --moxygen-bad SHA
#                                                 (--test CMD | --test-perf) [options]
#   bisect.sh measure [--moqx-rev REV] [--moxygen-rev SHA]
#                                                 (--test CMD | --test-perf) [options]
#
# Predicate (exactly one):
#   --test CMD             run CMD after a successful build; exit 0 is good,
#                          non-zero is bad. Runs from the repo root with
#                          MOQX_BISECT_{ROOT,BUILD_DIR,BINARY,MOQBIN,LOG} exported.
#   --test-perf            built-in relay-throughput classifier (see METRIC below)
#   --test-timeout SECS    wrap --test in `timeout --kill-after=10`. timeout's 124
#                          counts as bad, which is what you want when hunting a hang
#                          — pass --skip-code 124 to treat it as untestable instead.
#   --skip-code N          predicate exit code N means "untestable", not bad
#   --build-failure-is skip|bad
#                          verdict for a configure/build failure (default: skip).
#                          `bad` is for bisecting a build break itself.
#
# Build:
#   --profile NAME         build profile (default: default)
#   --moxygen-dir DIR      moxygen checkout to build (default: $MOQX_MOXYGEN_DIR)
#   --configure-args ARGS  extra args for configure.sh. Default is
#                          -DMOQX_BUILD_TESTS=OFF under --test-perf (nothing else
#                          needs them) and empty under --test, whose CMD may well
#                          be ctest.
#   -j, --jobs N           build parallelism, forwarded to configure.sh/build.sh
#   --pin-scripts REF      after every checkout, overlay scripts/perf,
#                          scripts/lib and scripts/moqx-run.sh from REF (resolved
#                          to a sha up front, since HEAD moves under a bisect).
#                          Use it when the measurement harness itself changed
#                          inside the range — otherwise each step measures with
#                          its own rev's script.
#
# --test-perf only:
#   --threshold MBPS       below this is bad. Default: measure the good and bad
#                          ends first and take their midpoint.
#   --perf-args ARGS       replace the default perf-test.sh arguments
#   --window LO:HI         AGGREGATE sample window, seconds (default 10:60) —
#                          change it with the run duration in --perf-args
#   --min-samples N        fewer usable samples than this is a skip (default 20)
#   --reps N               run N times per step and take the median (default 1)
#   --reps-near N          only when the first run lands within --near of the
#                          threshold, re-run to N reps and take the median
#   --near MBPS            near-threshold warning band (default 20)
#
# Reporting:
#   --results FILE         TSV of (phase, label, rev, value, verdict).
#                          Default: <logdir>/results.tsv
#   --logdir DIR           per-step logs (default: $MOQX_BISECT_LOGDIR, else
#                          .scratch/bisect-<timestamp>)
#   --dry-run              print the plan and exit without configuring or building
#   -h, --help
#
# PHASES — run them in order; each one narrows what the previous could not.
#
#   phase1  `git bisect run` over a moqx good..bad range. Every step parks the
#           moxygen checkout on THAT rev's MOXYGEN_REV by hand: configure.sh
#           --sync-moxygen-dir does not exist before moqx 71c223cc, so the
#           bisected rev's own configure.sh cannot be asked to do it.
#
#   phase2  when phase1 lands on a `sync: update moxygen` commit — one whose only
#           real change is MOXYGEN_REV — the bump conflates two things. This
#           splits them: build the LAST-GOOD moqx rev with its OWN moxygen
#           source, but with moxygen's dependency hashes forced forward to the
#           first-bad rev's. Good ⇒ the Meta stack is innocent and the change is
#           in moxygen's own source (phase3 next); bad ⇒ the culprit is inside
#           folly/fizz/wangle/mvfst/proxygen/picoquic and the next bisect is
#           there.
#
#   phase3  bisect inside moxygen — by cherry-pick replay, NOT `git bisect`.
#           Two independent reasons. Correctness: moxygen sync branches are
#           authored on an older base and merged, so a commit from inside one
#           does not build standalone and a plain bisect burns a full build to
#           reach exit 125. Speed: checking out such a commit perturbs the tree
#           enough that the superbuild reconfigures and recompiles folly and fizz
#           — measured ~1137 ninja edges against ~126 for a cherry-pick onto a
#           stable base. So: replay good..bad CUMULATIVELY onto a scratch branch
#           at the last-good rev and binary-search on how many commits to apply.
#           Commits that conflict applied alone often apply fine cumulatively.
#           The dependency hashes stay forced forward throughout, so the Meta
#           stack never rebuilds.
#
# METRIC (--test-perf). Not the perf client's "Throughput:" summary line: on
# older moxygen revs the client SIGSEGVs at teardown (fixed by moxygen 9a595e05)
# and never prints it. Instead the per-second [AGGREGATE] lines out of the run's
# client.log are averaged over --window. The <10000 Mbps filter drops a teardown
# counter underflow that emits ~4294939487 (2^32) — one such sample swamps the
# mean and makes everything score good.
#
# This mean-of-per-second-rates is NOT the statistic CI publishes as
# throughput_mbps (total_bytes/duration, ramp included). The two are not
# comparable; only compare bisect verdicts with each other.
#
# CAUTION: this drives plain `git`, not `sl`. It moves HEAD in the moqx repo and
# in the moxygen checkout, and restores both on exit. Both trees must be clean,
# and no `sl` command should run against either while it does — a plain-git
# checkout does not fire the sl update hook that normally parks moxygen on the
# pin, which is precisely why this script parks it itself.
set -uo pipefail
ROOT="${MOQX_BISECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

die()   { echo "bisect.sh: $*" >&2; exit 2; }
warn()  { echo "bisect.sh: $*" >&2; }
note()  { echo "== $*"; }
usage() { awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "${BASH_SOURCE[0]}"; exit "${1:-0}"; }

# git bisect run's contract, and this script's own verdict alphabet.
readonly V_GOOD=0 V_BAD=1 V_SKIP=125 V_ABORT=128

# ── Defaults ──────────────────────────────────────────────────────────────────
PROFILE="default"
MOXDIR="${MOQX_MOXYGEN_DIR:-}"
PREDICATE=""            # perf | cmd
TEST_CMD=""
TEST_TIMEOUT=""
SKIP_CODE=""
BUILD_FAILURE_IS="skip"
CONFIGURE_ARGS=""
CONFIGURE_ARGS_SET=0
JOBS=""
PIN_SCRIPTS=""
THRESHOLD=""
PERF_ARGS=""
WINDOW_LO=10 WINDOW_HI=60
MIN_SAMPLES=20
REPS=1
REPS_NEAR=0
NEAR=20
RESULTS=""
LOGDIR="${MOQX_BISECT_LOGDIR:-}"
DRY_RUN=0
GOOD="" BAD="" MOXYGEN_GOOD="" MOXYGEN_BAD="" MOQX_REV="" MOXYGEN_REV_ARG=""
STEP_PHASE="" STEP_LABEL=""

# Mirrors CI's nightly perf-test.yml with the duration cut to 60s.
PERF_ARGS_DEFAULT="-s 600 --ramp 150 -d 60 --io-threads 4 --threads 8"
PERF_CLIENT_ARGS_DEFAULT="--first_object_size=26516 --other_object_size=3788 --objects_per_group 60 --delivery_timeout 5000"

# The moxygen paths a cherry-pick may conflict on without implicating the code
# under test; a commit touching only these is reported and skipped in phase3.
readonly SKIPPABLE_PATHS='^(build/|\.github/|docker/|interop|scripts/|\.circleci/)'

# ── Arg parsing ───────────────────────────────────────────────────────────────
(($#)) || usage 2
CMD="$1"; shift
case "$CMD" in
  phase1|phase2|phase3|measure|__step) ;;
  -h|--help) usage ;;
  *) die "unknown command '$CMD' (phase1|phase2|phase3|measure; see --help)" ;;
esac

while (($#)); do
  case "$1" in
    --good)          (($# >= 2)) || die "$1 needs a rev"; GOOD="$2"; shift 2 ;;
    --bad)           (($# >= 2)) || die "$1 needs a rev"; BAD="$2"; shift 2 ;;
    --moxygen-good)  (($# >= 2)) || die "$1 needs a sha"; MOXYGEN_GOOD="$2"; shift 2 ;;
    --moxygen-bad)   (($# >= 2)) || die "$1 needs a sha"; MOXYGEN_BAD="$2"; shift 2 ;;
    --moqx-rev)      (($# >= 2)) || die "$1 needs a rev"; MOQX_REV="$2"; shift 2 ;;
    --moxygen-rev)   (($# >= 2)) || die "$1 needs a sha"; MOXYGEN_REV_ARG="$2"; shift 2 ;;
    --test)          (($# >= 2)) || die "$1 needs a command"; PREDICATE="cmd"; TEST_CMD="$2"; shift 2 ;;
    --test-perf)     PREDICATE="perf"; shift ;;
    --test-timeout)  (($# >= 2)) || die "$1 needs seconds"; TEST_TIMEOUT="$2"; shift 2 ;;
    --skip-code)     (($# >= 2)) || die "$1 needs an exit code"; SKIP_CODE="$2"; shift 2 ;;
    --build-failure-is) (($# >= 2)) || die "$1 needs skip|bad"; BUILD_FAILURE_IS="$2"; shift 2 ;;
    --profile)       (($# >= 2)) || die "$1 needs a profile"; PROFILE="$2"; shift 2 ;;
    --moxygen-dir)   (($# >= 2)) || die "$1 needs a directory"; MOXDIR="$2"; shift 2 ;;
    --configure-args) (($# >= 2)) || die "$1 needs arguments"; CONFIGURE_ARGS="$2"; CONFIGURE_ARGS_SET=1; shift 2 ;;
    -j|--jobs)       (($# >= 2)) || die "$1 needs a job count"; JOBS="$2"; shift 2 ;;
    --pin-scripts)   (($# >= 2)) || die "$1 needs a ref"; PIN_SCRIPTS="$2"; shift 2 ;;
    --threshold)     (($# >= 2)) || die "$1 needs a number"; THRESHOLD="$2"; shift 2 ;;
    --perf-args)     (($# >= 2)) || die "$1 needs arguments"; PERF_ARGS="$2"; shift 2 ;;
    --window)        (($# >= 2)) || die "$1 needs LO:HI"; WINDOW_LO="${2%%:*}"; WINDOW_HI="${2##*:}"; shift 2 ;;
    --min-samples)   (($# >= 2)) || die "$1 needs a count"; MIN_SAMPLES="$2"; shift 2 ;;
    --reps)          (($# >= 2)) || die "$1 needs a count"; REPS="$2"; shift 2 ;;
    --reps-near)     (($# >= 2)) || die "$1 needs a count"; REPS_NEAR="$2"; shift 2 ;;
    --near)          (($# >= 2)) || die "$1 needs a number"; NEAR="$2"; shift 2 ;;
    --results)       (($# >= 2)) || die "$1 needs a file"; RESULTS="$2"; shift 2 ;;
    --logdir)        (($# >= 2)) || die "$1 needs a directory"; LOGDIR="$2"; shift 2 ;;
    --dry-run)       DRY_RUN=1; shift ;;
    --step-phase)    (($# >= 2)) || die "$1 needs a phase"; STEP_PHASE="$2"; shift 2 ;;
    --step-label)    (($# >= 2)) || die "$1 needs a label"; STEP_LABEL="$2"; shift 2 ;;
    -h|--help)       usage ;;
    *)               die "unknown option '$1' (see --help)" ;;
  esac
done

[[ -n "$PREDICATE" ]] || die "choose a predicate: --test CMD (exit status decides) or --test-perf"
case "$BUILD_FAILURE_IS" in skip|bad) ;; *) die "--build-failure-is must be skip or bad" ;; esac
[[ -n "$SKIP_CODE" && ! "$SKIP_CODE" =~ ^[0-9]+$ ]] && die "--skip-code must be a number"
((REPS >= 1)) || die "--reps must be >= 1"
[[ "$PREDICATE" == cmd && -n "$THRESHOLD" ]] && die "--threshold applies to --test-perf only"

# --moxygen-dir is a fact about this machine, so the script is told rather than
# guessing; guessing could land on a CPM clone, which is CPM's to move.
[[ -n "$MOXDIR" ]] || die "no moxygen checkout — pass --moxygen-dir, or set MOQX_MOXYGEN_DIR"
MOXDIR="$(cd "$MOXDIR" 2>/dev/null && pwd)" || die "--moxygen-dir: '$MOXDIR' not found"
git -C "$MOXDIR" rev-parse --git-dir >/dev/null 2>&1 || die "$MOXDIR is not a git checkout"

if [[ "$PREDICATE" == perf && $CONFIGURE_ARGS_SET -eq 0 ]]; then
  CONFIGURE_ARGS="-DMOQX_BUILD_TESTS=OFF"
fi
[[ -n "$PERF_ARGS" ]] || PERF_ARGS="$PERF_ARGS_DEFAULT --client-args $(printf '%q' "$PERF_CLIENT_ARGS_DEFAULT")"

BUILD_DIR="$ROOT/build/$PROFILE"
MOQX_BIN="$BUILD_DIR/moqx"

# Created before anything can redirect into it: a `>` into a missing directory
# fails in the shell, before the command that would have made it ever runs.
if [[ -z "$LOGDIR" ]]; then
  LOGDIR="$ROOT/.scratch/bisect-$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "$LOGDIR" || die "cannot create log dir $LOGDIR"
LOGDIR="$(cd "$LOGDIR" && pwd)"
[[ -n "$RESULTS" ]] || RESULTS="$LOGDIR/results.tsv"
mkdir -p "$(dirname "$RESULTS")" || die "cannot create results dir"
[[ -s "$RESULTS" ]] || printf 'phase\tlabel\trev\tvalue\tverdict\n' > "$RESULTS"

# The step runner is re-invoked by `git bisect run` with the moqx worktree
# already moved to some other rev, where this file may not exist (or may be a
# different version). A copy outside the tree is what git actually runs.
SELF_COPY="$LOGDIR/bisect-runner.sh"

# ── git helpers ───────────────────────────────────────────────────────────────
git_clean() {  # $1 = repo
  local out
  out="$(git -C "$1" status --porcelain --untracked-files=no 2>/dev/null)" || return 1
  [[ -z "$out" ]]
}

# The name to return to. A branch survives the detach; otherwise only the sha does.
saved_head() {  # $1 = repo
  git -C "$1" symbolic-ref -q --short HEAD 2>/dev/null || git -C "$1" rev-parse HEAD
}

resolve() {  # $1 = repo, $2 = rev
  git -C "$1" rev-parse -q --verify "$2^{commit}" 2>/dev/null
}

short() { printf '%.12s' "$1"; }

# Reads MOXYGEN_REV at an arbitrary moqx ref without checking it out.
# print-pin.cmake is the supported reader for the CHECKED-OUT tree only, and it
# arrived in the same commit as dependencies.cmake, so an older rev has neither.
pin_at() {  # $1 = moqx ref
  git -C "$ROOT" show "$1:cmake/dependencies.cmake" 2>/dev/null |
    sed -n 's/^[[:space:]]*set(MOXYGEN_REV[[:space:]]*"\([0-9a-f]\{40\}\)").*/\1/p' | head -1
}

pin_here() {
  local p
  p="$(cd "$ROOT" && cmake -DPIN=MOXYGEN_REV -P cmake/print-pin.cmake 2>/dev/null)"
  [[ "$p" =~ ^[0-9a-f]{40}$ ]] && { printf '%s' "$p"; return 0; }
  pin_at HEAD
}

# ── moxygen tree management ───────────────────────────────────────────────────
# Forced hash files leave the tree dirty, and git checkout / cherry-pick / bisect
# all abort on a dirty tree — so every ref change is preceded by a restore.
DEP_HASHES_FORCED=0
dep_hashes_restore() {
  ((DEP_HASHES_FORCED)) || return 0
  git -C "$MOXDIR" checkout -- build/deps/github_hashes 2>/dev/null
  DEP_HASHES_FORCED=0
}

dep_hashes_force() {  # $1 = moxygen ref whose hashes to adopt
  local ref="$1" f
  dep_hashes_restore
  while read -r f; do
    [[ -n "$f" ]] || continue
    git -C "$MOXDIR" show "$ref:$f" > "$MOXDIR/$f" || return 1
  done < <(git -C "$MOXDIR" ls-tree -r --name-only "$ref" -- build/deps/github_hashes |
             grep -- '-rev\.txt$')
  DEP_HASHES_FORCED=1
}

dep_digest() {
  cat "$MOXDIR"/build/deps/github_hashes/*/*-rev.txt 2>/dev/null | cksum | awk '{print $1}'
}

moxygen_park() {  # $1 = sha
  local sha="$1"
  dep_hashes_restore
  [[ "$(git -C "$MOXDIR" rev-parse HEAD)" == "$sha" ]] && return 0
  git -C "$MOXDIR" rev-parse -q --verify "$sha^{commit}" >/dev/null 2>&1 ||
    git -C "$MOXDIR" fetch --quiet origin "$sha" 2>/dev/null ||
    { warn "moxygen $(short "$sha") is not fetchable in $MOXDIR"; return 1; }
  git -C "$MOXDIR" checkout --quiet --detach "$sha" || { warn "cannot check out moxygen $(short "$sha")"; return 1; }
}

# ── pinned measurement scripts ────────────────────────────────────────────────
# A bisect checkout swaps scripts/ along with everything else. build.sh and
# configure.sh SHOULD move with the rev (each rev builds itself), but the
# measurement harness must not, or steps are not comparable.
readonly PINNED_PATHS=(scripts/perf scripts/lib scripts/moqx-run.sh)
SCRIPTS_PINNED=0
pinned_scripts_restore() {
  ((SCRIPTS_PINNED)) || return 0
  git -C "$ROOT" checkout HEAD -- "${PINNED_PATHS[@]}" 2>/dev/null
  SCRIPTS_PINNED=0
}
pinned_scripts_apply() {
  [[ -n "$PIN_SCRIPTS" ]] || return 0
  git -C "$ROOT" checkout "$PIN_SCRIPTS" -- "${PINNED_PATHS[@]}" 2>/dev/null || return 1
  SCRIPTS_PINNED=1
}

# ── build ─────────────────────────────────────────────────────────────────────
BUILD_START=0
build_here() {  # $1 = step log
  local log="$1" jflag=()
  [[ -n "$JOBS" ]] && jflag=(-j "$JOBS")
  BUILD_START=$(date +%s)
  # sleep 1: mtime has 1s granularity on some filesystems, so a binary written in
  # the same second as BUILD_START would not compare as newer.
  sleep 1
  {
    echo "--- configure $(date -u +%FT%TZ)"
    # shellcheck disable=SC2086 # CONFIGURE_ARGS is a user-supplied argument string
    (cd "$ROOT" && ./scripts/configure.sh "$PROFILE" --moxygen from-source \
        --moxygen-dir "$MOXDIR" ${jflag[@]+"${jflag[@]}"} $CONFIGURE_ARGS)
  } >>"$log" 2>&1 || { echo "configure failed (see $log)"; return 1; }
  {
    echo "--- build $(date -u +%FT%TZ)"
    (cd "$ROOT" && ./scripts/build.sh "$PROFILE" ${jflag[@]+"${jflag[@]}"})
  } >>"$log" 2>&1 || { echo "build failed (see $log)"; return 1; }
}

moqbin_dir() {
  local MOQBIN=""
  # shellcheck disable=SC1091 # generated by the moqx configure
  [[ -f "$BUILD_DIR/moqx-tools.env" ]] && . "$BUILD_DIR/moqx-tools.env"
  printf '%s' "$MOQBIN"
}

# The artifacts the predicate consumes are part of the measurement loop, not
# "just test code" — moxygen 9a595e05 and 3ab33bf0 both change the perf tools.
# If BUILD_MOQTEST/BUILD_SAMPLES were ever off, or an install step silently
# no-op'd, they would persist from a previous build and every later verdict
# would be taken with a stale instrument. So: they must exist, and whenever the
# moxygen inputs changed since the last successful step they must be newer than
# this build started. Unchanged inputs legitimately leave them untouched.
artifacts_ok() {  # $@ = artifact paths
  local identity prev="" p stale=0
  identity="$(git -C "$MOXDIR" rev-parse HEAD)-$(dep_digest)"
  # No previous step means no point of comparison: the first build legitimately
  # relinks nothing if the tree was already built at this state.
  [[ -f "$LOGDIR/last-identity" ]] && prev="$(cat "$LOGDIR/last-identity")"
  for p in "$@"; do
    if [[ ! -e "$p" ]]; then
      warn "measurement artifact missing: $p"
      return 1
    fi
    if [[ -n "$prev" && "$identity" != "$prev" ]] &&
       (( $(stat -c %Y "$p" 2>/dev/null || echo 0) < BUILD_START )); then
      warn "STALE measurement artifact: $p predates this build, but the moxygen"
      warn "  inputs changed ($(short "${prev:-none}") -> $(short "$identity")) — it was not rebuilt."
      stale=1
    fi
  done
  ((stale)) && return 1
  printf '%s' "$identity" > "$LOGDIR/last-identity"
}

# ── predicates ────────────────────────────────────────────────────────────────
LAST_VALUE="-"

# Averages the per-second [AGGREGATE] Mbps out of one perf run. Echoes the mean
# on stdout; returns 1 when the run yielded too few usable samples.
perf_run_once() {  # $1 = step log
  local log="$1" out rundir stats n mean
  out="$( (cd "$ROOT" && eval "./scripts/perf/perf-test.sh --relay $(printf '%q' "$MOQX_BIN") $PERF_ARGS") 2>&1 )"
  printf '%s\n' "$out" >> "$log"
  # perf-test.sh prints the run directory on the way in AND from its EXIT trap,
  # so the path is recoverable even when the client crashes before summarising.
  rundir="$(printf '%s\n' "$out" | sed -n -E 's/^Logs( saved to)?:? (\/.*)$/\2/p' | tail -1)"
  [[ -n "$rundir" && -f "$rundir/client.log" ]] || { warn "no client.log from perf-test.sh"; return 1; }
  echo "--- samples from $rundir" >> "$log"
  stats="$(grep 'AGGREGATE' "$rundir/client.log" |
    sed -E 's/.*\[([0-9]+)s\].*Mbps: ([0-9.]+).*/\1 \2/' |
    awk -v lo="$WINDOW_LO" -v hi="$WINDOW_HI" \
      '$1>=lo && $1<=hi && $2<10000 {n++; s+=$2} END{printf "%d %.2f", n, (n?s/n:0)}')"
  n="${stats%% *}"; mean="${stats##* }"
  echo "samples=$n mean=$mean" >> "$log"
  if ((n < MIN_SAMPLES)); then
    warn "only $n usable samples (need $MIN_SAMPLES) — untestable"
    return 1
  fi
  printf '%s' "$mean"
}

median() {
  local -a s; local n
  mapfile -t s < <(printf '%s\n' "$@" | sort -g)
  n=${#s[@]}
  if ((n % 2)); then printf '%s' "${s[n/2]}"
  else awk -v a="${s[n/2-1]}" -v b="${s[n/2]}" 'BEGIN{printf "%.2f", (a+b)/2}'
  fi
}

# Echoes the classified value into LAST_VALUE; returns a V_* verdict.
predicate_perf() {  # $1 = step log
  local log="$1" v reps="$REPS"
  local -a vals=()
  v="$(perf_run_once "$log")" || return "$V_SKIP"
  vals+=("$v")
  # A verdict is one 60s sample; observed run-to-run sd is ~3 Mbps good / ~6 bad.
  if ((REPS == 1)) && ((REPS_NEAR > 1)) && [[ -n "$THRESHOLD" ]] &&
     awk -v v="$v" -v t="$THRESHOLD" -v n="$NEAR" 'BEGIN{exit !(v-t<n && t-v<n)}'; then
    note "$v is within $NEAR Mbps of $THRESHOLD — re-running to $REPS_NEAR reps"
    reps="$REPS_NEAR"
  fi
  while ((${#vals[@]} < reps)); do
    v="$(perf_run_once "$log")" || return "$V_SKIP"
    vals+=("$v")
  done
  LAST_VALUE="$(median "${vals[@]}")"
  [[ -n "$THRESHOLD" ]] || { warn "no threshold set"; return "$V_ABORT"; }
  if awk -v v="$LAST_VALUE" -v t="$THRESHOLD" -v n="$NEAR" 'BEGIN{exit !(v-t<n && t-v<n)}'; then
    warn "$LAST_VALUE Mbps is within $NEAR of the $THRESHOLD threshold — verdict is low confidence"
    warn "  (re-run this rev with --reps 3, or move the threshold)"
  fi
  awk -v v="$LAST_VALUE" -v t="$THRESHOLD" 'BEGIN{exit !(v>=t)}' && return "$V_GOOD"
  return "$V_BAD"
}

predicate_cmd() {  # $1 = step log
  local log="$1" rc moqbin
  moqbin="$(moqbin_dir)"
  echo "--- test: $TEST_CMD" >> "$log"
  # shellcheck disable=SC2094 # CMD is handed the log path deliberately; it appends
  (
    cd "$ROOT" || exit 127
    export MOQX_BISECT_ROOT="$ROOT" MOQX_BISECT_BUILD_DIR="$BUILD_DIR" \
           MOQX_BISECT_BINARY="$MOQX_BIN" MOQX_BISECT_MOQBIN="$moqbin" \
           MOQX_BISECT_LOG="$log"
    if [[ -n "$TEST_TIMEOUT" ]]; then
      timeout --kill-after=10 "$TEST_TIMEOUT" bash -c "$TEST_CMD"
    else
      bash -c "$TEST_CMD"
    fi
  ) >>"$log" 2>&1
  rc=$?
  LAST_VALUE="exit=$rc"
  [[ -n "$SKIP_CODE" && "$rc" == "$SKIP_CODE" ]] && return "$V_SKIP"
  ((rc == 0)) && return "$V_GOOD"
  return "$V_BAD"
}

record() {  # phase label rev value verdict
  printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" >> "$RESULTS"
}

verdict_name() {
  case "$1" in "$V_GOOD") echo good ;; "$V_BAD") echo bad ;; "$V_SKIP") echo skip ;; *) echo abort ;; esac
}

# One measurement at whatever the two trees currently hold. Returns a V_* code.
run_step() {  # $1 = phase, $2 = label, $3 = rev shown in the results file
  local phase="$1" label="$2" rev="$3" log rc
  log="$LOGDIR/$phase-$label.log"
  LAST_VALUE="-"
  note "[$phase] $label ($(short "$rev")) moxygen=$(short "$(git -C "$MOXDIR" rev-parse HEAD)") -> $log"

  if ((DRY_RUN)); then
    echo "  would: configure.sh $PROFILE --moxygen from-source --moxygen-dir $MOXDIR $CONFIGURE_ARGS"
    echo "  would: build.sh $PROFILE"
    [[ "$PREDICATE" == perf ]] && echo "  would: perf-test.sh --relay $MOQX_BIN $PERF_ARGS"
    [[ "$PREDICATE" == cmd  ]] && echo "  would: $TEST_CMD"
    return "$V_SKIP"
  fi

  if ! build_here "$log"; then
    rc=$([[ "$BUILD_FAILURE_IS" == bad ]] && echo "$V_BAD" || echo "$V_SKIP")
    record "$phase" "$label" "$rev" "build-failed" "$(verdict_name "$rc")"
    note "  build failed -> $(verdict_name "$rc")"
    return "$rc"
  fi

  local moqbin; moqbin="$(moqbin_dir)"
  local -a artifacts=("$MOQX_BIN")
  if [[ "$PREDICATE" == perf ]]; then
    [[ -n "$moqbin" ]] || { warn "MOQBIN is empty in $BUILD_DIR/moqx-tools.env"; return "$V_SKIP"; }
    artifacts+=("$moqbin/moqperf_test_client" "$moqbin/moqtest_server")
  elif [[ -n "$moqbin" ]]; then
    artifacts+=("$moqbin")
  fi
  if ! artifacts_ok "${artifacts[@]}"; then
    record "$phase" "$label" "$rev" "stale-artifacts" skip
    return "$V_SKIP"
  fi

  if [[ "$PREDICATE" == perf ]]; then predicate_perf "$log"; else predicate_cmd "$log"; fi
  rc=$?
  record "$phase" "$label" "$rev" "$LAST_VALUE" "$(verdict_name "$rc")"
  note "  $LAST_VALUE -> $(verdict_name "$rc")"
  return "$rc"
}

# ── phase 1 ───────────────────────────────────────────────────────────────────
# Invoked by `git bisect run` with the moqx worktree already at the rev under
# test. Restores everything it forced before returning, so bisect's next
# checkout does not abort on a dirty tree.
cmd_step() {
  local rev pin rc
  rev="$(git -C "$ROOT" rev-parse HEAD)"
  trap 'dep_hashes_restore; pinned_scripts_restore' EXIT

  pin="$(pin_here)"
  if [[ ! "$pin" =~ ^[0-9a-f]{40}$ ]]; then
    warn "no MOXYGEN_REV at $(short "$rev") — untestable"
    exit "$V_SKIP"
  fi
  if ((!DRY_RUN)); then
    moxygen_park "$pin" || exit "$V_SKIP"
    pinned_scripts_apply || { warn "--pin-scripts $PIN_SCRIPTS failed"; exit "$V_ABORT"; }
  fi

  run_step "${STEP_PHASE:-p1}" "${STEP_LABEL:-$(short "$rev")}" "$rev"
  rc=$?
  exit "$rc"
}

cmd_phase1() {
  local good bad rc bisect_log culprit
  [[ -n "$GOOD" && -n "$BAD" ]] || die "phase1 needs --good REV and --bad REV"
  good="$(resolve "$ROOT" "$GOOD")" || die "--good '$GOOD' is not a moqx commit"
  bad="$(resolve "$ROOT" "$BAD")"   || die "--bad '$BAD' is not a moqx commit"

  note "phase1: git bisect over $(short "$good")..$(short "$bad") ($(git -C "$ROOT" rev-list --count "$good..$bad") commits)"
  note "  good pin: $(short "$(pin_at "$good")")   bad pin: $(short "$(pin_at "$bad")")"

  if [[ "$PREDICATE" == perf && -z "$THRESHOLD" ]]; then
    reference_threshold "$good" "$bad" || return $?
  fi
  ((DRY_RUN)) && { note "dry run: would bisect and run the step above at each rev"; return 0; }

  cp "$SELF" "$SELF_COPY" || die "cannot stage the step runner in $LOGDIR"
  chmod +x "$SELF_COPY" || die "cannot make the step runner executable"

  git -C "$ROOT" bisect start "$bad" "$good" >>"$LOGDIR/phase1-bisect.log" 2>&1 ||
    die "git bisect start failed (see $LOGDIR/phase1-bisect.log)"
  BISECTING=1
  bisect_log="$LOGDIR/phase1-bisect.log"
  # git bisect run's own exit status reports the wrapper, not the step, so the
  # transcript is what actually says whether a first-bad commit was found.
  MOQX_BISECT_ROOT="$ROOT" MOQX_BISECT_LOGDIR="$LOGDIR" \
    git -C "$ROOT" bisect run "$SELF_COPY" __step "${STEP_ARGS[@]}" --step-phase p1 \
      2>&1 | tee -a "$bisect_log"
  rc=${PIPESTATUS[1]}
  culprit="$(grep -oE '^[0-9a-f]{40} is the first bad commit' "$bisect_log" | tail -1 | cut -d' ' -f1)"
  if [[ -z "$culprit" ]]; then
    warn "phase1 did not isolate a commit (git bisect run exited $rc); see $bisect_log"
    return 1
  fi
  PHASE1_CULPRIT="$culprit"
  note "phase1 first-bad: $culprit"
  git -C "$ROOT" log -1 --format='  %h %s' "$culprit"
  if pin_only_change "$culprit"; then
    note "  this commit changes only MOXYGEN_REV — run phase2 next:"
    note "    bisect.sh phase2 --good $culprit^ --bad $culprit $(printf '%s ' "${STEP_ARGS[@]}")"
  fi
}

# True when the commit's only substantive change is the moxygen pin, i.e. a
# `sync: update moxygen` commit that phase2 exists to take apart.
pin_only_change() {  # $1 = moqx rev
  local files
  files="$(git -C "$ROOT" show --name-only --format= "$1" | grep -v '^$')"
  [[ -n "$files" ]] || return 1
  ! printf '%s\n' "$files" | grep -qv -E '^(cmake/dependencies\.cmake|\.github/|docs/)'
}

# ── threshold reference (perf only) ───────────────────────────────────────────
PHASE1_CULPRIT="" REF_GOOD="" REF_BAD=""
reference_threshold() {  # $1 = good moqx rev, $2 = bad moqx rev
  local good="$1" bad="$2"
  note "no --threshold: measuring the good and bad ends first"
  ((DRY_RUN)) && { note "  would measure $(short "$good") and $(short "$bad")"; return 0; }
  at_moqx_rev "$good" ref-good "$good" || return 1
  REF_GOOD="$LAST_VALUE"
  at_moqx_rev "$bad" ref-bad "$bad" || return 1
  REF_BAD="$LAST_VALUE"
  [[ "$REF_GOOD" =~ ^[0-9.]+$ && "$REF_BAD" =~ ^[0-9.]+$ ]] ||
    die "reference runs did not both produce a number (good=$REF_GOOD bad=$REF_BAD)"
  THRESHOLD="$(awk -v a="$REF_GOOD" -v b="$REF_BAD" 'BEGIN{printf "%.2f", (a+b)/2}')"
  note "reference: good=$REF_GOOD bad=$REF_BAD -> threshold=$THRESHOLD"
  awk -v a="$REF_GOOD" -v b="$REF_BAD" -v n="$NEAR" 'BEGIN{exit !((a-b<2*n) && (b-a<2*n))}' &&
    warn "the two ends are less than $((2 * NEAR)) Mbps apart — this signal may be too weak to bisect"
}

# Moves the moqx worktree to a rev, parks moxygen on its pin, and measures.
at_moqx_rev() {  # $1 = moqx rev, $2 = label, $3 = rev recorded
  local pin
  dep_hashes_restore; pinned_scripts_restore
  git -C "$ROOT" checkout --quiet --detach "$1" || { warn "cannot check out moqx $1"; return 1; }
  pin="$(pin_here)"
  [[ "$pin" =~ ^[0-9a-f]{40}$ ]] || { warn "no MOXYGEN_REV at $1"; return 1; }
  moxygen_park "$pin" || return 1
  pinned_scripts_apply || return 1
  run_step "${STEP_PHASE:-ref}" "$2" "$3"
}

# ── phase 2 ───────────────────────────────────────────────────────────────────
cmd_phase2() {
  local good bad pin_good pin_bad rc
  [[ -n "$GOOD" && -n "$BAD" ]] || die "phase2 needs --good REV and --bad REV (the moqx revs either side of the pin bump)"
  good="$(resolve "$ROOT" "$GOOD")" || die "--good '$GOOD' is not a moqx commit"
  bad="$(resolve "$ROOT" "$BAD")"   || die "--bad '$BAD' is not a moqx commit"
  pin_good="$(pin_at "$good")"; pin_bad="$(pin_at "$bad")"
  [[ "$pin_good" =~ ^[0-9a-f]{40}$ ]] || die "no MOXYGEN_REV at --good"
  [[ "$pin_bad"  =~ ^[0-9a-f]{40}$ ]] || die "no MOXYGEN_REV at --bad"
  [[ "$pin_good" != "$pin_bad" ]] || die "--good and --bad pin the same moxygen; phase2 has nothing to split"

  note "phase2: moxygen SOURCE $(short "$pin_good") + dependency hashes from $(short "$pin_bad")"
  git -C "$MOXDIR" diff --stat "$pin_good" "$pin_bad" -- build/deps/github_hashes | sed 's/^/  /'
  if ((DRY_RUN)); then
    note "dry run: would build moqx $(short "$good") against that combination and test it"
    return 0
  fi

  dep_hashes_restore; pinned_scripts_restore
  git -C "$ROOT" checkout --quiet --detach "$good" || die "cannot check out moqx $(short "$good")"
  moxygen_park "$pin_good" || die "cannot park moxygen at $(short "$pin_good")"
  dep_hashes_force "$pin_bad" || die "cannot force dependency hashes from $(short "$pin_bad")"
  pinned_scripts_apply || die "--pin-scripts failed"

  run_step p2 "deps-forward" "$pin_good+deps@$(short "$pin_bad")"
  rc=$?
  case "$rc" in
    "$V_GOOD")
      note "phase2: GOOD — the Meta dependency stack is innocent; the change is in moxygen's own source."
      note "  next: bisect.sh phase3 --moxygen-good $pin_good --moxygen-bad $pin_bad $(printf '%s ' "${STEP_ARGS[@]}")" ;;
    "$V_BAD")
      note "phase2: BAD — old moxygen source + new dependency hashes reproduces it."
      note "  the culprit is in the Meta stack; bisect these ranges next:"
      git -C "$MOXDIR" diff "$pin_good" "$pin_bad" -- build/deps/github_hashes |
        grep -E '^[-+]Subproject|^\+\+\+ ' | sed 's/^/    /' ;;
    *) warn "phase2 was inconclusive ($(verdict_name "$rc"))" ;;
  esac
  return "$rc"
}

# ── phase 3 ───────────────────────────────────────────────────────────────────
PHASE3_BRANCH="moqx-bisect-replay"
PHASE3_SKIPPED=()

# Resets the scratch branch to the last-good rev and replays the first N commits
# of the range onto it, cumulatively. Individual commits from a merged sync
# branch may conflict applied alone and apply cleanly after their predecessors,
# which is exactly why this is a replay and not a bisect.
phase3_apply() {  # $1 = N, $2 = log
  local n="$1" log="$2" i sha files
  dep_hashes_restore
  git -C "$MOXDIR" cherry-pick --quit >/dev/null 2>&1
  git -C "$MOXDIR" checkout --quiet -B "$PHASE3_BRANCH" "$MOXYGEN_GOOD" >>"$log" 2>&1 ||
    { warn "cannot reset $PHASE3_BRANCH"; return 1; }
  PHASE3_SKIPPED=()
  for ((i = 0; i < n; i++)); do
    sha="${RANGE[i]}"
    # --empty=keep: a commit already contained in the base becomes empty here,
    # which would otherwise stop the sequence mid-replay.
    if git -C "$MOXDIR" cherry-pick --allow-empty --empty=keep "$sha" >>"$log" 2>&1; then
      continue
    fi
    files="$(git -C "$MOXDIR" show --name-only --format= "$sha" | grep -v '^$')"
    if [[ -n "$files" ]] && ! printf '%s\n' "$files" | grep -qvE "$SKIPPABLE_PATHS"; then
      # Build/CI/interop-only: it cannot be the culprit for a runtime predicate,
      # and it is reported rather than dropped silently.
      git -C "$MOXDIR" cherry-pick --skip >>"$log" 2>&1 ||
        git -C "$MOXDIR" cherry-pick --abort >>"$log" 2>&1
      PHASE3_SKIPPED+=("$sha")
      continue
    fi
    git -C "$MOXDIR" cherry-pick --abort >>"$log" 2>&1
    warn "cherry-pick of $(short "$sha") conflicts and touches source files — cannot replay past it"
    git -C "$MOXDIR" log -1 --format='  %h %s' "$sha" >&2
    return 1
  done
  # Forced last: cherry-pick refuses to run on a dirty tree, so the hashes go
  # forward only once every ref change for this step is done.
  dep_hashes_force "$MOXYGEN_BAD" || return 1
}

phase3_eval() {  # $1 = N  -> V_* code
  local n="$1" log="$LOGDIR/p3-n$1.log" rc tip
  : > "$log"
  if ! phase3_apply "$n" "$log"; then
    record p3 "n$n" "replay-failed" "-" skip
    return "$V_SKIP"
  fi
  ((${#PHASE3_SKIPPED[@]})) && note "  skipped (build/CI-only, conflicting): ${PHASE3_SKIPPED[*]}"
  # Not RANGE[n-1]: bash reads a negative index from the end of the array, so
  # n=0 would report the LAST commit of the range as the one under test.
  if ((n == 0)); then tip="$MOXYGEN_GOOD"; else tip="${RANGE[n - 1]}"; fi
  run_step p3 "n$n" "$tip"
  rc=$?
  return "$rc"
}

cmd_phase3() {
  local total lo hi mid rc probe
  [[ -n "$MOXYGEN_GOOD" && -n "$MOXYGEN_BAD" ]] || die "phase3 needs --moxygen-good SHA and --moxygen-bad SHA"
  MOXYGEN_GOOD="$(resolve "$MOXDIR" "$MOXYGEN_GOOD")" || die "--moxygen-good is not a commit in $MOXDIR"
  MOXYGEN_BAD="$(resolve "$MOXDIR" "$MOXYGEN_BAD")"   || die "--moxygen-bad is not a commit in $MOXDIR"

  mapfile -t RANGE < <(git -C "$MOXDIR" log --reverse --topo-order --no-merges \
                         --format=%H "$MOXYGEN_GOOD..$MOXYGEN_BAD")
  total=${#RANGE[@]}
  ((total)) || die "no commits in $(short "$MOXYGEN_GOOD")..$(short "$MOXYGEN_BAD")"
  note "phase3: cherry-pick replay of $total commits onto $(short "$MOXYGEN_GOOD"), dependency hashes forced to $(short "$MOXYGEN_BAD")"
  git -C "$MOXDIR" log --reverse --topo-order --no-merges --format='  %h %s' \
    "$MOXYGEN_GOOD..$MOXYGEN_BAD" | head -40

  # moqx itself is held still: phase3 varies only moxygen.
  if [[ -n "$MOQX_REV" ]]; then
    ((DRY_RUN)) || { dep_hashes_restore; pinned_scripts_restore
      git -C "$ROOT" checkout --quiet --detach "$MOQX_REV" || die "cannot check out moqx $MOQX_REV"; }
  fi
  if ((DRY_RUN)); then
    note "dry run: would binary-search n in [0,$total] — ~$(awk -v t="$total" 'BEGIN{printf "%d", log(t+1)/log(2)+1}') builds"
    return 0
  fi
  pinned_scripts_apply || die "--pin-scripts failed"

  # n=0 is the last-good source with the new dependency hashes: the same build
  # phase2 called good, re-run here as this search's own lower bound.
  phase3_eval 0; rc=$?
  ((rc == V_GOOD)) || warn "phase3 lower bound (n=0) scored $(verdict_name "$rc"), not good — the range or the threshold is wrong"
  phase3_eval "$total"; rc=$?
  ((rc == V_BAD)) || warn "phase3 upper bound (n=$total) scored $(verdict_name "$rc"), not bad — the replay may not reproduce the original tree"

  lo=0 hi=$total
  while ((hi - lo > 1)); do
    mid=$(( (lo + hi) / 2 ))
    phase3_eval "$mid"; rc=$?
    if ((rc == V_SKIP)); then
      # Nudge off an untestable n rather than abandoning the search.
      probe=$((mid + 1))
      while ((probe < hi)); do
        phase3_eval "$probe"; rc=$?
        ((rc != V_SKIP)) && { mid=$probe; break; }
        probe=$((probe + 1))
      done
      ((rc == V_SKIP)) && { warn "phase3 stalled: every n in ($lo,$hi) is untestable"; return 1; }
    fi
    if ((rc == V_GOOD)); then lo=$mid; else hi=$mid; fi
  done
  note "phase3 first-bad: ${RANGE[hi-1]}"
  git -C "$MOXDIR" log -1 --format='  %h %s' "${RANGE[hi-1]}"
  ((${#PHASE3_SKIPPED[@]})) &&
    note "  NB: these commits conflicted and were skipped as build/CI-only: ${PHASE3_SKIPPED[*]}"
  PHASE3_CULPRIT="${RANGE[hi-1]}"
}

# ── measure ───────────────────────────────────────────────────────────────────
cmd_measure() {
  local pin rc
  dep_hashes_restore; pinned_scripts_restore
  if [[ -n "$MOQX_REV" ]]; then
    git -C "$ROOT" checkout --quiet --detach "$MOQX_REV" || die "cannot check out moqx $MOQX_REV"
  fi
  pin="${MOXYGEN_REV_ARG:-$(pin_here)}"
  [[ "$pin" =~ ^[0-9a-f]{40}$ ]] || die "no moxygen rev (pass --moxygen-rev, or check out a moqx rev with a pin)"
  ((DRY_RUN)) && { note "dry run: would build moqx $(short "$(git -C "$ROOT" rev-parse HEAD)") against moxygen $(short "$pin")"; return 0; }
  moxygen_park "$pin" || die "cannot park moxygen at $(short "$pin")"
  pinned_scripts_apply || die "--pin-scripts failed"
  run_step measure "$(short "$(git -C "$ROOT" rev-parse HEAD)")" "$(git -C "$ROOT" rev-parse HEAD)"
  rc=$?
  note "measure: $LAST_VALUE ($(verdict_name "$rc"))"
  return "$rc"
}

# ── entry ─────────────────────────────────────────────────────────────────────
# The step child must NOT restore HEAD or reset the bisect — that is the parent's
# job, and doing it here would end the bisect after its first step.
if [[ "$CMD" == __step ]]; then
  cmd_step
fi

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
PHASE3_CULPRIT=""
BISECTING=0
MOQX_WAS="" MOXYGEN_WAS=""

# The options a step re-invocation needs; --good/--bad and the phase are not
# among them, since the step tests whatever HEAD git bisect chose.
STEP_ARGS=(--moxygen-dir "$MOXDIR" --profile "$PROFILE" --build-failure-is "$BUILD_FAILURE_IS"
           --logdir "$LOGDIR" --results "$RESULTS" --window "$WINDOW_LO:$WINDOW_HI"
           --min-samples "$MIN_SAMPLES" --reps "$REPS" --near "$NEAR")
[[ -n "$JOBS" ]]        && STEP_ARGS+=(-j "$JOBS")
[[ -n "$PIN_SCRIPTS" ]] && STEP_ARGS+=(--pin-scripts "$PIN_SCRIPTS")
[[ -n "$SKIP_CODE" ]]   && STEP_ARGS+=(--skip-code "$SKIP_CODE")
[[ -n "$TEST_TIMEOUT" ]] && STEP_ARGS+=(--test-timeout "$TEST_TIMEOUT")
((REPS_NEAR > 1))       && STEP_ARGS+=(--reps-near "$REPS_NEAR")
if [[ "$PREDICATE" == perf ]]; then STEP_ARGS+=(--test-perf --perf-args "$PERF_ARGS")
else STEP_ARGS+=(--test "$TEST_CMD"); fi
[[ $CONFIGURE_ARGS_SET -eq 1 ]] && STEP_ARGS+=(--configure-args "$CONFIGURE_ARGS")

if ((!DRY_RUN)); then
  git_clean "$ROOT"   || die "$ROOT is dirty — this moves HEAD; commit or stash first"
  git_clean "$MOXDIR" || die "$MOXDIR is dirty — this moves HEAD; commit or stash first"
  MOQX_WAS="$(saved_head "$ROOT")"
  MOXYGEN_WAS="$(saved_head "$MOXDIR")"
fi

# One trap for every way out. Order matters: the forced files must go before any
# ref change, or the checkouts below abort on a dirty tree.
cleanup() {
  local rc=$?
  dep_hashes_restore
  pinned_scripts_restore
  if ((BISECTING)); then git -C "$ROOT" bisect reset >/dev/null 2>&1; BISECTING=0; fi
  if [[ -n "$MOXYGEN_WAS" ]]; then
    git -C "$MOXDIR" cherry-pick --quit >/dev/null 2>&1
    git -C "$MOXDIR" checkout --quiet "$MOXYGEN_WAS" >/dev/null 2>&1 ||
      warn "could not restore $MOXDIR to $MOXYGEN_WAS"
    git -C "$MOXDIR" branch -q -D "$PHASE3_BRANCH" >/dev/null 2>&1
  fi
  if [[ -n "$MOQX_WAS" ]]; then
    git -C "$ROOT" checkout --quiet "$MOQX_WAS" >/dev/null 2>&1 ||
      warn "could not restore $ROOT to $MOQX_WAS"
  fi
  echo
  echo "results: $RESULTS   logs: $LOGDIR"
  [[ -n "$PHASE1_CULPRIT" ]] && echo "phase1 identified moqx $PHASE1_CULPRIT"
  [[ -n "$PHASE3_CULPRIT" ]] && echo "phase3 identified moxygen $PHASE3_CULPRIT"
  exit "$rc"
}
trap cleanup EXIT

case "$CMD" in
  phase1)  cmd_phase1 ;;
  phase2)  cmd_phase2 ;;
  phase3)  cmd_phase3 ;;
  measure) cmd_measure ;;
esac
