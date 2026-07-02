#!/usr/bin/env bash
# perf-test-ci.sh — CI orchestration wrapper for cross-machine perf testing.
#
# Deploys binaries to dedicated VMs, runs perf-test.sh remotely on the relay
# VM with the client running on a separate client VM, collects metrics, and
# produces a JSON results file for trend tracking.
#
# Environment variables (typically set from GitHub Actions secrets):
#   PERF_RELAY_HOST       — SSH host for the relay VM (user@ip)
#   PERF_CLIENT_HOST      — SSH host for the client VM (user@ip)
#   PERF_SSH_KEY_FILE     — Path to SSH private key file
#   PERF_RELAY_PORT       — Relay QUIC port (default: 4433)
#   PERF_ADMIN_PORT       — Relay admin port (default: 19701)
#
# Usage: scripts/perf-test-ci.sh [options]
#   --binary PATH         Path to moqx binary (default: build/moqx)
#   --moqbin PATH         Path to moxygen bin dir (default: .scratch/moxygen-install/bin)
#   --output PATH         Output JSON file (default: perf-results.json)
#   --subscriber-max N    Max subscribers (default: 1000)
#   --ramp N              Subscribers/sec (default: 100)
#   --duration N          Test duration seconds (default: 120)
#   --io-threads N        Relay IO threads (default: 4)
#   --client-threads N    Client threads (default: 4)
#   --client-args ARGS    Extra flags appended to moqperf_test_client
#                         e.g. --client-args "--first_object_size=424242 --other_object_size=60606"
#   --warmup N            Seconds to skip after the client starts before
#                         sampling relay CPU/network metrics, so the subscriber
#                         ramp and congestion control have settled
#                         (default: subscriber_max/ramp + 10)
#   --cooldown N          Seconds before the client finishes to stop sampling,
#                         excluding teardown noise (default: 5)
#
# The relay config is rendered on the relay VM by scripts/moqx-run.sh from
# scripts/config.bench.yaml — the SAME path scripts/perf-test.sh uses — so the
# two harnesses stay in lockstep and CI trend data can't silently drift.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"

# ── Defaults ───────────────────────────────────────────────────────────────────
BINARY="${BINARY:-$REPO/build/moqx}"
MOQBIN="${MOQBIN:-$REPO/.scratch/moxygen-install/bin}"
OUTPUT="perf-results.json"
SUBSCRIBER_MAX=1000
RAMP=100
DURATION=120
IO_THREADS=4
CLIENT_THREADS=4
DELIVERY_TIMEOUT=500
TRANSPORT="quic"
CLIENT_EXTRA_ARGS=()
WARMUP=""          # empty = auto (subscriber_max/ramp + 10); see below
COOLDOWN=5

RELAY_HOST="${PERF_RELAY_HOST:-}"
CLIENT_HOST="${PERF_CLIENT_HOST:-}"
SSH_KEY_FILE="${PERF_SSH_KEY_FILE:-}"
RELAY_PORT="${PERF_RELAY_PORT:-4433}"
ADMIN_PORT="${PERF_ADMIN_PORT:-19701}"

# ── Arg parsing ────────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary)          BINARY="$2";          shift 2 ;;
    --moqbin)          MOQBIN="$2";          shift 2 ;;
    --output)          OUTPUT="$2";          shift 2 ;;
    --subscriber-max)  SUBSCRIBER_MAX="$2";  shift 2 ;;
    --ramp)            RAMP="$2";            shift 2 ;;
    --duration)        DURATION="$2";        shift 2 ;;
    --io-threads)      IO_THREADS="$2";      shift 2 ;;
    --client-threads)  CLIENT_THREADS="$2";  shift 2 ;;
    --client-args)     read -ra CLIENT_EXTRA_ARGS <<< "$2"; shift 2 ;;
    --delivery-timeout) DELIVERY_TIMEOUT="$2"; shift 2 ;;
    --transport)       TRANSPORT="$2";       shift 2 ;;
    --warmup)          WARMUP="$2";          shift 2 ;;
    --cooldown)        COOLDOWN="$2";        shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

# ── Validation ─────────────────────────────────────────────────────────────────
if [[ -z "$RELAY_HOST" ]]; then
  echo "ERROR: PERF_RELAY_HOST not set" >&2; exit 1
fi
if [[ -z "$CLIENT_HOST" ]]; then
  echo "ERROR: PERF_CLIENT_HOST not set" >&2; exit 1
fi

SSH_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=10 -o BatchMode=yes)
[[ -n "$SSH_KEY_FILE" ]] && SSH_OPTS+=(-i "$SSH_KEY_FILE")

if [[ "$RAMP" -le 0 ]]; then
  echo "ERROR: --ramp must be > 0" >&2; exit 1
fi

# Auto measurement warmup: wait out the ramp (subscriber_max/ramp) plus a 10s
# settle margin for congestion control before we start averaging CPU/network.
if [[ -z "$WARMUP" ]]; then
  WARMUP=$(( SUBSCRIBER_MAX / RAMP + 10 ))
fi

MOQTEST_SERVER="$MOQBIN/moqtest_server"
MOQPERF_CLIENT="$MOQBIN/moqperf_test_client"

RELAY_RUN_SCRIPT="$REPO/scripts/moqx-run.sh"
RELAY_CONFIG_TEMPLATE="$REPO/scripts/config.bench.yaml"

for f in "$BINARY" "$MOQTEST_SERVER" "$MOQPERF_CLIENT" "$RELAY_RUN_SCRIPT" "$RELAY_CONFIG_TEMPLATE"; do
  if [[ ! -f "$f" ]]; then
    echo "ERROR: not found: $f" >&2; exit 1
  fi
done

# ── Collect shared libraries ────────────────────────────────────────────────────
echo "Collecting shared library dependencies..."
LOCAL_LIBDIR="/tmp/moqx-perf-libs-$$"
mkdir -p "$LOCAL_LIBDIR"
bash "$REPO/scripts/collect-libs.sh" "$BINARY" "$LOCAL_LIBDIR" > /dev/null
bash "$REPO/scripts/collect-libs.sh" "$MOQTEST_SERVER" "$LOCAL_LIBDIR" > /dev/null
bash "$REPO/scripts/collect-libs.sh" "$MOQPERF_CLIENT" "$LOCAL_LIBDIR" > /dev/null
echo "Libraries collected: $(ls $LOCAL_LIBDIR/*.so* 2>/dev/null | wc -l) files"

# ── Git metadata ───────────────────────────────────────────────────────────────
COMMIT_SHA="${GITHUB_SHA:-$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)}"
COMMIT_SHORT="${COMMIT_SHA:0:7}"
BRANCH="${GITHUB_REF_NAME:-$(git -C "$REPO" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)}"
TIMESTAMP="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

echo "═══════════════════════════════════════════════════════════"
echo " moqx Performance Test (CI)"
echo "═══════════════════════════════════════════════════════════"
echo "  Commit:       $COMMIT_SHORT ($BRANCH)"
echo "  Relay VM:     $RELAY_HOST"
echo "  Client VM:    $CLIENT_HOST"
echo "  Subscribers:  $SUBSCRIBER_MAX (ramp $RAMP/s)"
echo "  Duration:     ${DURATION}s"
echo "  IO threads:   $IO_THREADS"
echo "  Transport:    $TRANSPORT"
[[ ${#CLIENT_EXTRA_ARGS[@]} -gt 0 ]] && echo "  Client args:  ${CLIENT_EXTRA_ARGS[*]}"
echo "  Warmup:       ${WARMUP}s (skip after client start)"
echo "  Cooldown:     ${COOLDOWN}s (skip before client end)"
echo "═══════════════════════════════════════════════════════════"

# ── Remote directory setup ─────────────────────────────────────────────────────
REMOTE_DIR="/tmp/moqx-perf-ci"

timeout 5 ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "mkdir -p $REMOTE_DIR" || true
timeout 5 ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "pkill -f 'moqx.*perf-ci' 2>/dev/null || true; pkill -f moqtest_server 2>/dev/null || true" || true
timeout 5 ssh "${SSH_OPTS[@]}" "$CLIENT_HOST" "mkdir -p $REMOTE_DIR" || true
timeout 5 ssh "${SSH_OPTS[@]}" "$CLIENT_HOST" "pkill -f moqperf_test_client 2>/dev/null || true" || true

# ── Deploy binaries ────────────────────────────────────────────────────────────
echo "Deploying binaries..."
rsync -az -e "ssh ${SSH_OPTS[*]}" "$BINARY" "${RELAY_HOST}:${REMOTE_DIR}/moqx"
rsync -az -e "ssh ${SSH_OPTS[*]}" "$MOQTEST_SERVER" "${RELAY_HOST}:${REMOTE_DIR}/moqtest_server"
rsync -az -e "ssh ${SSH_OPTS[*]}" "$MOQPERF_CLIENT" "${CLIENT_HOST}:${REMOTE_DIR}/moqperf_test_client"
rsync -az -e "ssh ${SSH_OPTS[*]}" "$REPO/scripts/perf-metrics.sh" "${RELAY_HOST}:${REMOTE_DIR}/perf-metrics.sh"
rsync -az -e "ssh ${SSH_OPTS[*]}" "$RELAY_RUN_SCRIPT" "${RELAY_HOST}:${REMOTE_DIR}/moqx-run.sh"
rsync -az -e "ssh ${SSH_OPTS[*]}" "$RELAY_CONFIG_TEMPLATE" "${RELAY_HOST}:${REMOTE_DIR}/config.bench.yaml"
rsync -az -e "ssh ${SSH_OPTS[*]}" "$LOCAL_LIBDIR/" "${RELAY_HOST}:${REMOTE_DIR}/lib/"
rsync -az -e "ssh ${SSH_OPTS[*]}" "$LOCAL_LIBDIR/" "${CLIENT_HOST}:${REMOTE_DIR}/lib/"
echo "Deploy complete"

# moqx-run.sh renders config.bench.yaml with envsubst; make sure it's installed.
if ! ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "command -v envsubst >/dev/null 2>&1"; then
  echo "ERROR: envsubst not found on $RELAY_HOST (install gettext-base)" >&2; exit 1
fi

# ── Cleanup trap ───────────────────────────────────────────────────────────────
cleanup() {
  echo "Cleaning up remote processes..."
  timeout 5 ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "pkill -f '${REMOTE_DIR}/moqx' 2>/dev/null || true; pkill -f '${REMOTE_DIR}/moqtest_server' 2>/dev/null || true; pkill -f '${REMOTE_DIR}/perf-metrics.sh' 2>/dev/null || true" 2>/dev/null || true
  timeout 5 ssh "${SSH_OPTS[@]}" "$CLIENT_HOST" "pkill -f '${REMOTE_DIR}/moqperf_test_client' 2>/dev/null || true" 2>/dev/null || true
}
trap cleanup EXIT

# ── Start relay (via moqx-run.sh + config.bench.yaml on the relay VM) ──────────
# Mirrors scripts/perf-test.sh's relay launch so both harnesses share identical
# tuning (thread count, flow control, UDP buffer, bbr2, GSO, recv batch). This
# is what makes --io-threads actually take effect and stops CI trend drift.
echo "Starting relay on $RELAY_HOST (io_threads=$IO_THREADS)..."
ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "
  ulimit -n 65536 2>/dev/null || true
  nohup env LD_LIBRARY_PATH=${REMOTE_DIR}/lib \
    MOQX_RELAY_ID=perf-ci-relay \
    MOQX_RESOLVED_CONFIG=${REMOTE_DIR}/relay.yaml \
    bash ${REMOTE_DIR}/moqx-run.sh \
      --insecure --no-cache --ignore-path-mtu \
      --bin        ${REMOTE_DIR}/moqx \
      --config     ${REMOTE_DIR}/config.bench.yaml \
      --bind       :: \
      --port       ${RELAY_PORT} \
      --admin-port ${ADMIN_PORT} \
      --endpoint   /moq-relay \
      --threads    ${IO_THREADS} \
      --cc         bbr2 \
      --relay-thread --no-local-forwarders \
      > ${REMOTE_DIR}/relay.log 2>&1 &
  echo \$!
" > /tmp/relay_pid.txt
RELAY_PID=$(cat /tmp/relay_pid.txt | tr -d '[:space:]')
echo "Relay PID: $RELAY_PID"

# Wait for relay to be ready
echo "Waiting for relay admin endpoint..."
DEADLINE=$((SECONDS + 15))
until ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "curl -sf http://localhost:${ADMIN_PORT}/info >/dev/null 2>&1"; do
  if ((SECONDS >= DEADLINE)); then
    echo "ERROR: relay not ready after 15s" >&2
    ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "tail -20 ${REMOTE_DIR}/relay.log" >&2
    exit 1
  fi
  sleep 0.5
done
echo "Relay ready"

# ── Start metrics poller ──────────────────────────────────────────────────────
# Anchor the poller's t=0 to the relay VM clock so we can map the metrics.log
# 'elapsed' column onto the client run and window out the warmup/cooldown.
POLLER_START_EPOCH=$(ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "date +%s")
echo "Starting metrics poller..."
ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "
  nohup bash ${REMOTE_DIR}/perf-metrics.sh $ADMIN_PORT ${REMOTE_DIR}/metrics.log > ${REMOTE_DIR}/metrics_stderr.log 2>&1 &
"

# ── Start publisher ───────────────────────────────────────────────────────────
RELAY_URL="https://127.0.0.1:${RELAY_PORT}/moq-relay"
QUIC_FLAG="--quic_transport=true"
[[ "$TRANSPORT" == "webtransport" ]] && QUIC_FLAG="--quic_transport=false"

echo "Starting moqtest_server on $RELAY_HOST..."
ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "
  nohup env LD_LIBRARY_PATH=${REMOTE_DIR}/lib ${REMOTE_DIR}/moqtest_server --relay_url='${RELAY_URL}' ${QUIC_FLAG} --include_timestamp_extension=true > ${REMOTE_DIR}/server.log 2>&1 &
"

# Wait for publisher to connect
echo "Waiting for publisher connection..."
DEADLINE=$((SECONDS + 15))
until ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "
  curl -sf http://localhost:${ADMIN_PORT}/metrics 2>/dev/null \
    | grep '^moqx_moqActiveSessions ' | awk '\$2 >= 1 {found=1} END {exit !found}'
" 2>/dev/null; do
  if ((SECONDS >= DEADLINE)); then
    echo "ERROR: publisher did not connect after 15s" >&2
    ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "tail -20 ${REMOTE_DIR}/server.log" >&2
    exit 1
  fi
  sleep 0.5
done
echo "Publisher connected"

# ── Capture pre-test system state ─────────────────────────────────────────────
PRE_RSS=$(ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "
  cat /proc/${RELAY_PID}/status 2>/dev/null | grep VmRSS | awk '{print \$2}' || echo 0
")

# ── Run performance test client ───────────────────────────────────────────────
RELAY_IP=$(ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "hostname -I | awk '{print \$1}'")
CLIENT_RELAY_URL="https://${RELAY_IP}:${RELAY_PORT}/moq-relay"

echo "Running moqperf_test_client on $CLIENT_HOST..."
echo "  relay_url=$CLIENT_RELAY_URL"
echo "  subscribers=$SUBSCRIBER_MAX, ramp=$RAMP/s, duration=${DURATION}s"
echo "---"

# Relay-VM clock at client start/end anchors the measurement window (see below).
MEASURE_START_EPOCH=$(ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "date +%s")

# Preserve spaces/special chars when forwarding extra client flags over ssh.
CLIENT_EXTRA_ARGS_ESCAPED=""
if [[ ${#CLIENT_EXTRA_ARGS[@]} -gt 0 ]]; then
  printf -v CLIENT_EXTRA_ARGS_ESCAPED ' %q' "${CLIENT_EXTRA_ARGS[@]}"
fi

CLIENT_OUTPUT=$(ssh "${SSH_OPTS[@]}" "$CLIENT_HOST" "
  ulimit -n 65536 2>/dev/null || true
  env LD_LIBRARY_PATH=${REMOTE_DIR}/lib ${REMOTE_DIR}/moqperf_test_client \
    --relay_url='${CLIENT_RELAY_URL}' \
    ${QUIC_FLAG} \
    --subscriber_max=${SUBSCRIBER_MAX} \
    --subscriber_ramp=${RAMP} \
    --duration=${DURATION} \
    --delivery_timeout=${DELIVERY_TIMEOUT} \
    --num_threads=${CLIENT_THREADS} \
    ${CLIENT_EXTRA_ARGS_ESCAPED} \
    2>&1
" | tee /tmp/perf-client-output.txt)

echo "---"
echo "Client finished"

MEASURE_END_EPOCH=$(ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "date +%s")

# ── Capture post-test system state ────────────────────────────────────────────
POST_RSS=$(ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "
  cat /proc/${RELAY_PID}/status 2>/dev/null | grep VmRSS | awk '{print \$2}' || echo 0
")

# ── Collect + window the metrics log ──────────────────────────────────────────
# The poller samples the whole run — idle pre-connect, the subscriber ramp, and
# teardown — so averaging every row biases CPU/throughput low. Window to the
# steady-state slice [client_start + warmup, client_end - cooldown], expressed
# in the metrics.log 'elapsed' column (seconds since POLLER_START_EPOCH). All
# three epochs come from the relay VM clock, so this is skew-free.
ssh "${SSH_OPTS[@]}" "$RELAY_HOST" "cat ${REMOTE_DIR}/metrics.log 2>/dev/null" > /tmp/perf-metrics-full.log || true

WIN_START=$(( (MEASURE_START_EPOCH - POLLER_START_EPOCH) + WARMUP ))
WIN_END=$(( (MEASURE_END_EPOCH - POLLER_START_EPOCH) - COOLDOWN ))

if [[ -s /tmp/perf-metrics-full.log ]]; then
  awk -F'\t' -v s="$WIN_START" -v e="$WIN_END" \
    'NR==1 { print; next } ($1+0) >= s && ($1+0) <= e' \
    /tmp/perf-metrics-full.log > /tmp/perf-metrics.log
  # Fall back to the full log if the window captured no data rows (e.g. a short
  # run where warmup+cooldown overlap), so downstream stats aren't empty.
  if [[ "$(wc -l < /tmp/perf-metrics.log)" -le 1 ]]; then
    echo "WARNING: measurement window [$WIN_START,$WIN_END]s captured no rows; using full log" >&2
    cp /tmp/perf-metrics-full.log /tmp/perf-metrics.log
  fi
else
  : > /tmp/perf-metrics.log
fi

echo "Windowed metrics: $(( $(wc -l < /tmp/perf-metrics.log) - 1 )) of $(( $(wc -l < /tmp/perf-metrics-full.log) - 1 )) samples (window ${WIN_START}-${WIN_END}s)"

# Average a named column of the windowed metrics log. Args: <col header> <fmt>
metrics_col_avg() {
  local header="$1" fmt="$2"
  awk -F'\t' -v h="$header" -v fmt="$fmt" '
    NR==1 { for (i=1;i<=NF;i++) if ($i==h) c=i; next }
    c && $c ~ /^[0-9.]+$/ { sum+=$c; n++ }
    END { if (n>0) printf fmt, sum/n; else printf fmt, 0 }
  ' /tmp/perf-metrics.log
}

# Average relay CPU% and external (non-loopback) throughput over the window.
CPU_AVG=$(metrics_col_avg "CPU%" "%.1f")
NET_THROUGHPUT=$(metrics_col_avg "ext_Mbps" "%.1f")

# ── Parse results and generate JSON ──────────────────────────────────────────
echo "Generating results JSON..."
bash "$REPO/scripts/perf-results-to-json.sh" \
  --client-output /tmp/perf-client-output.txt \
  --metrics-log /tmp/perf-metrics.log \
  --commit "$COMMIT_SHA" \
  --branch "$BRANCH" \
  --timestamp "$TIMESTAMP" \
  --subscriber-max "$SUBSCRIBER_MAX" \
  --ramp "$RAMP" \
  --duration "$DURATION" \
  --io-threads "$IO_THREADS" \
  --client-threads "$CLIENT_THREADS" \
  --client-window-start "$WIN_START" \
  --client-window-end "$WIN_END" \
  --relay-cpu "$CPU_AVG" \
  --relay-rss-kb "$POST_RSS" \
  --net-throughput-mbps "$NET_THROUGHPUT" \
  --delivery-timeout "$DELIVERY_TIMEOUT" \
  --transport "$TRANSPORT" \
  --output "$OUTPUT"

echo "═══════════════════════════════════════════════════════════"
echo " Results written to: $OUTPUT"
echo "═══════════════════════════════════════════════════════════"
cat "$OUTPUT"

# Cleanup local lib directory
rm -rf "$LOCAL_LIBDIR"
