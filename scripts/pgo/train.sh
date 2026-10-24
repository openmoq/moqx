#!/usr/bin/env bash
# train.sh — train a GCC PGO profile by driving an instrumented moqx over loopback.
#
# Runs the relay as deployed (local forwarders, 4 IO threads, relay thread, cache)
# on mvfst and on picoquic, over raw QUIC and WebTransport, under moqtest_server
# and moqperf_test_client load. SIGTERM stops the relay so libgcov writes its
# counters. The publisher and client come from the same instrumented stack, so
# they write theirs under a discard GCOV_PREFIX. Scenarios share the profile
# dir, which libgcov sums into; it is emptied first.
#
# Usage:
#   scripts/pgo/train.sh [--relay PATH] [--moqbin DIR] [--profile-dir DIR]
#                        [--work-dir DIR] [--duration SECONDS] [--subscribers N]
#
#   --relay PATH        instrumented moqx (default: build/release-pgo/moqx)
#   --moqbin DIR        moxygen bin dir (default: from moqx-tools.env beside the relay)
#   --profile-dir DIR   profile dir (default: MOQX_PGO_DIR from the relay's build)
#   --work-dir DIR      logs and discarded counters (default: mktemp)
#   --duration SECONDS  base duration per scenario (default: 60)
#   --subscribers N     peak subscribers per scenario (default: 200)
#
# Env: GCOV_DUMP names the gcov-dump matching the compiler (default: gcov-dump).
#
# Fails when a scenario fails, when the relay does not exit on SIGTERM, or when
# a hot object of moqx, moxygen, mvfst, picoquic or folly has no non-zero counters.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
RELAY="$REPO/build/release-pgo/moqx"
MOQBIN="${MOQBIN:-}"
PROFILE_DIR=""
WORK_DIR=""
DURATION=60
SUBSCRIBERS=200
IO_THREADS=4
CLIENT_THREADS=4
PORT=4433
ADMIN_PORT=19701
ENDPOINT=/moq-relay
GCOV_DUMP="${GCOV_DUMP:-gcov-dump}"

# Columns: name, quic stack, transport, duration and subscriber percent, first/
# other object bytes, objects per group. Object sizes are the perf profiles'
# 1mbps and 4mbps; the 4mbps runs take a quarter of the subscribers so both
# carry ~200 Mbps, which the instrumented relay serves without timeouts.
SCENARIOS=(
  "mvfst-quic     mvfst     quic          100  100  26516   3788   60"
  "mvfst-wt       mvfst     webtransport  50   25   106064  26516  60"
  "picoquic-quic  picoquic  quic          100  100  26516   3788   60"
  "picoquic-wt    picoquic  webtransport  50   25   106064  26516  60"
)

# One translation unit per layer whose counters prove its hot path ran.
HOT_OBJECTS=(MoqxRelay.cpp MoQSession.cpp QuicTransportFunctions.cpp sender.c EventBase.cpp)

usage() { awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"; exit "${1:-0}"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --relay)        RELAY="$2"; shift 2 ;;
    --moqbin)       MOQBIN="$2"; shift 2 ;;
    --profile-dir)  PROFILE_DIR="$2"; shift 2 ;;
    --work-dir)     WORK_DIR="$2"; shift 2 ;;
    --duration)     DURATION="$2"; shift 2 ;;
    --subscribers)  SUBSCRIBERS="$2"; shift 2 ;;
    -h|--help)      usage 0 ;;
    *) echo "Unknown option: $1" >&2; usage 1 ;;
  esac
done

log() { printf '[pgo-train] %s\n' "$*"; }
die() { log "ERROR: $*" >&2; exit 1; }

[[ -x "$RELAY" ]] || die "relay $RELAY missing or not executable"
BUILD_DIR="$(cd "$(dirname "$RELAY")" && pwd)"
if [[ -z "$MOQBIN" && -f "$BUILD_DIR/moqx-tools.env" ]]; then
  # shellcheck disable=SC1091
  source "$BUILD_DIR/moqx-tools.env"
fi
if [[ -z "$PROFILE_DIR" && -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  PROFILE_DIR="$(sed -n 's/^MOQX_PGO_DIR:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt")"
  grep -q '^MOQX_PGO:[A-Z]*=generate$' "$BUILD_DIR/CMakeCache.txt" \
    || die "$BUILD_DIR is not a MOQX_PGO=generate build"
fi
[[ -n "$PROFILE_DIR" ]] || die "no --profile-dir and no MOQX_PGO_DIR in $BUILD_DIR/CMakeCache.txt"
for bin in moqtest_server moqperf_test_client; do
  [[ -x "${MOQBIN:-}/$bin" ]] || die "${MOQBIN:-<unset>}/$bin missing; pass --moqbin"
done
for tool in ss curl openssl envsubst "$GCOV_DUMP"; do
  command -v "$tool" >/dev/null || die "$tool is required"
done

[[ -n "$WORK_DIR" ]] || WORK_DIR="$(mktemp -d)"
mkdir -p "$WORK_DIR" "$PROFILE_DIR"
find "$PROFILE_DIR" -name '*.gcda' -delete
DISCARD="$WORK_DIR/discard"
RELAY_URL="https://127.0.0.1:${PORT}${ENDPOINT}"

# picoquic refuses an insecure listener; nothing verifies the cert.
CERT="$WORK_DIR/cert.pem" KEY="$WORK_DIR/key.pem"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$KEY" -out "$CERT" \
  -days 1 -subj "/CN=localhost" >/dev/null 2>&1 || die "could not mint a TLS cert"

RELAY_PID=""
PUB_PID=""
cleanup() {
  if [[ -n "$PUB_PID" ]]; then kill "$PUB_PID" 2>/dev/null || true; fi
  if [[ -n "$RELAY_PID" ]]; then kill "$RELAY_PID" 2>/dev/null || true; fi
}
trap cleanup EXIT

wait_until() {  # seconds, command...
  local deadline=$(( SECONDS + $1 )); shift
  until "$@"; do
    (( SECONDS < deadline )) || return 1
    sleep 0.5
  done
}
relay_ready() { curl -sf "http://127.0.0.1:${ADMIN_PORT}/info" >/dev/null 2>&1; }
publisher_connected() {
  local n
  n=$(curl -sf "http://127.0.0.1:${ADMIN_PORT}/metrics" 2>/dev/null \
      | awk '$1 == "moqx_moqActiveSessions" {print $2}')
  [[ -n "$n" ]] && (( ${n%.*} >= 1 ))
}

# SIGTERM, then wait: the relay returns from main and libgcov writes on exit.
# Past 10 s the shutdown watchdog writes the counters itself and exits 1.
stop_relay() {
  local log=$1 deadline=$(( SECONDS + 30 )) rc=0
  kill -TERM "$RELAY_PID"
  while kill -0 "$RELAY_PID" 2>/dev/null; do
    if (( SECONDS >= deadline )); then
      kill -KILL "$RELAY_PID" 2>/dev/null || true
      die "relay did not exit within 30s of SIGTERM; counters not written"
    fi
    sleep 0.5
  done
  wait "$RELAY_PID" || rc=$?
  RELAY_PID=""
  if (( rc != 0 )); then
    grep -q 'Shutdown timed out' "$log" || die "relay exited with status $rc (see $log)"
    log "relay teardown hit the watchdog; its counters were written before exit"
  fi
}

run_scenario() {
  local name=$1 stack=$2 transport=$3 dur_pct=$4 subs_pct=$5 first=$6 other=$7 per_group=$8
  local dur=$(( DURATION * dur_pct / 100 )) subs=$(( SUBSCRIBERS * subs_pct / 100 ))
  (( dur >= 10 )) || dur=10
  (( subs >= 1 )) || subs=1
  local ramp=$(( subs / 4 ))
  (( ramp >= 1 )) || ramp=1
  local dir="$WORK_DIR/$name"
  mkdir -p "$dir"
  log "scenario $name: $stack/$transport subscribers=$subs ramp=$ramp/s duration=${dur}s objects=${first}/${other} x ${per_group}"

  local cc=bbr2 tls=(--insecure) tflag=--transport=quic
  [[ "$stack" == picoquic ]] && cc=bbr tls=(--cert "$CERT" --key "$KEY")
  [[ "$transport" == webtransport ]] && tflag=--transport=h3wt

  MOQX_RELAY_ID="pgo-train" MOQX_RESOLVED_CONFIG="$dir/relay.yaml" \
    "$REPO/scripts/moqx-run.sh" --env /dev/null \
      --bin "$RELAY" --config "$REPO/scripts/perf/config.bench.yaml" \
      --bind 127.0.0.1 --port "$PORT" --admin-port "$ADMIN_PORT" --endpoint "$ENDPOINT" \
      --threads "$IO_THREADS" --local-forwarders --relay-thread --cache --ignore-path-mtu \
      --quic-stack "$stack" --cc "$cc" "${tls[@]}" \
      >"$dir/relay.log" 2>&1 &
  RELAY_PID=$!     # moqx-run execs moqx
  wait_until 30 relay_ready || die "relay not ready (see $dir/relay.log)"

  GCOV_PREFIX="$DISCARD" "$MOQBIN/moqtest_server" \
    --relay_url="$RELAY_URL" "$tflag" --include_timestamp_extension=true \
    >"$dir/publisher.log" 2>&1 &
  PUB_PID=$!
  wait_until 20 publisher_connected || die "publisher did not connect (see $dir/publisher.log)"

  local client_rc=0
  GCOV_PREFIX="$DISCARD" timeout $(( dur + 60 )) "$MOQBIN/moqperf_test_client" \
    --relay_url="$RELAY_URL" "$tflag" \
    --subscriber_max="$subs" --subscriber_ramp="$ramp" \
    --duration="$dur" --delivery_timeout=5000 --num_threads="$CLIENT_THREADS" \
    --first_object_size="$first" --other_object_size="$other" \
    --objects_per_group="$per_group" \
    >"$dir/client.log" 2>&1 || client_rc=$?

  kill -TERM "$PUB_PID" 2>/dev/null || true
  wait "$PUB_PID" 2>/dev/null || true
  PUB_PID=""
  stop_relay "$dir/relay.log"

  if (( client_rc != 0 )); then
    tail -n 20 "$dir/client.log" >&2
    die "scenario $name: client exited with status $client_rc"
  fi
  grep -F '[AGGREGATE]' "$dir/client.log" | tail -n 1 || true
  grep -qE 'Total Objects: *[1-9]' "$dir/client.log" \
    || die "scenario $name: client reports no objects received (see $dir/client.log)"
}

for row in "${SCENARIOS[@]}"; do
  # shellcheck disable=SC2086
  run_scenario $row
done

# Every linked object gets a .gcda, executed or not; gcov-dump marks the
# untouched ones "(all zero)".
count=$(find "$PROFILE_DIR" -name '*.gcda' | wc -l)
(( count > 0 )) || die "no .gcda files under $PROFILE_DIR"
for obj in "${HOT_OBJECTS[@]}"; do
  hit=0
  while IFS= read -r f; do
    # grep -c reads to the end: an early-exiting grep -q fails the pipe under pipefail.
    n=$("$GCOV_DUMP" -l "$f" 2>/dev/null | grep -E 'COUNTERS arcs [0-9]+ counts' | grep -vc 'all zero' || true)
    if (( n > 0 )); then
      hit=1
      break
    fi
  done < <(find "$PROFILE_DIR" -name "*#${obj}.gcda")
  (( hit )) || die "no non-zero counters for $obj under $PROFILE_DIR"
done
log "profile: $count .gcda files, $(du -sh "$PROFILE_DIR" | cut -f1), counters in ${HOT_OBJECTS[*]}"
