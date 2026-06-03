#!/usr/bin/env bash
# perf-test.sh — relay throughput/subscriber perf test
#
# Starts a moqx relay, moqtest_server (publisher), and moqperf_test_client
# (subscriber ramp), then prints the client's output.  Logs for all three
# processes are always saved to /tmp/moqx-perf-<timestamp>/.
#
# Usage: scripts/perf-test.sh [options]
#   --relay PATH           Path to moqx binary (default: build/moqx)
#   --moqbin PATH          Path to moxygen bin dir
#                          (default: .scratch/moxygen-install/bin)
#   --subscriber-max N     Max total subscribers (default: 500)
#   --ramp N               Subscribers added per second (default: 100)
#   --duration N           Test duration in seconds (default: 30)
#   --delivery-timeout N   Delivery timeout in ms (default: 500)
#   --transport TYPE       quic or webtransport (default: quic)
#   --no-relay-thread      Disable relay exec thread (use_relay_thread: false)
#   --io-threads N         Number of relay IO threads (default: 1)
#   --threads N            Number of perf client threads (default: 2)
#   --relay-log SPEC       folly XLOG config passed as --logging=SPEC to relay
#   --bpf-steering         Enable mvfst BPF reuseport steering (requires MOQX_ENABLE_BPF_STEERING build)
#   --no-bpf-steering      Disable mvfst BPF reuseport steering (default)
#   --jemalloc             LD_PRELOAD jemalloc (auto-detected from /lib64/libjemalloc.so.2)
#   --metrics              Run perf-metrics.sh alongside the relay; logs to LOG_DIR/metrics.log
#   --perf-duration N      Run perf record -F 499 -g -e cycles on relay for N seconds; starts after
#                          subscribers finish ramping (delay = 3*max/ramp s); saves to LOG_DIR/
#   --perf-events EVENTS   Run a second perf record -F 99 -g with these events alongside --perf-duration
#                          e.g. --perf-events cache-misses,dTLB-load-misses; saves to LOG_DIR/perf-events.data
#   --perf-stat            Run perf stat for the full test duration; saves to LOG_DIR/perf-stat.txt
#   --trace-script PATH    Run PATH <relay_pid> for the duration; output → LOG_DIR/trace.log
#   --client-args ARGS     Extra flags appended to moqperf_test_client invocation
#                          e.g. --client-args "--first_object_size=5000 --object_size=1400"
#   --remote-client HOST   Run moqperf_test_client on HOST via ssh instead of
#                          locally.  The binary is expected at
#                          /tmp/moqperf_test_client on the remote host.
#                          HOST may include a user prefix (user@host).
#   --udp-socket-buffer-bytes N
#                          UDP socket send/recv buffer size in bytes for the
#                          relay listener (defaults to net.core.wmem_max).
#
# Linux-only options (not supported on macOS):
#   --metrics, --perf-duration, --perf-events, --perf-stat, --jemalloc,
#   --remote-client

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"

# ── Defaults ───────────────────────────────────────────────────────────────────
BINARY="${RELAY:-$REPO/build/moqx}"
MOQBIN="${MOQBIN:-$REPO/.scratch/moxygen-install/bin}"
SUBSCRIBER_MAX=500
RAMP=100
DURATION=30
DELIVERY_TIMEOUT=500
TRANSPORT="quic"
USE_RELAY_THREAD="true"
IO_THREADS=1
CLIENT_THREADS=2
RELAY_LOG_SPEC=""
BPF_STEERING="false"
JEMALLOC=""
RUN_METRICS=false
PERF_DURATION=0
PERF_EVENTS=""
RUN_PERF_STAT=false
REMOTE_CLIENT_HOST=""
UDP_SOCKET_BUFFER_BYTES=$(cat /proc/sys/net/core/wmem_max 2>/dev/null || echo 1048576)
TRACE_SCRIPT=""
CLIENT_EXTRA_ARGS=()

RELAY_PORT=4433
RELAY_ADMIN_PORT=19701
ENDPOINT="/moq-relay"

# ── Arg parsing ────────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --relay)            BINARY="$2";            shift 2 ;;
    --moqbin)           MOQBIN="$2";            shift 2 ;;
    --subscriber-max)   SUBSCRIBER_MAX="$2";    shift 2 ;;
    --ramp)             RAMP="$2";              shift 2 ;;
    --duration)         DURATION="$2";          shift 2 ;;
    --delivery-timeout) DELIVERY_TIMEOUT="$2";  shift 2 ;;
    --transport)        TRANSPORT="$2";         shift 2 ;;
    --no-relay-thread)  USE_RELAY_THREAD="false"; shift ;;
    --io-threads)       IO_THREADS="$2";          shift 2 ;;
    --threads)          CLIENT_THREADS="$2";      shift 2 ;;
    --relay-log)        RELAY_LOG_SPEC="$2";      shift 2 ;;
    --bpf-steering)     BPF_STEERING="true";      shift ;;
    --no-bpf-steering)  BPF_STEERING="false";     shift ;;
    --jemalloc)         JEMALLOC="auto";           shift ;;
    --metrics)          RUN_METRICS=true;          shift ;;
    --perf-duration)    PERF_DURATION="$2";        shift 2 ;;
    --perf-events)      PERF_EVENTS="$2";          shift 2 ;;
    --perf-stat)        RUN_PERF_STAT=true;        shift ;;
    --trace-script)     TRACE_SCRIPT="$2";         shift 2 ;;
    --client-args)      read -ra CLIENT_EXTRA_ARGS <<< "$2"; shift 2 ;;
    --remote-client)    REMOTE_CLIENT_HOST="$2";  shift 2 ;;
    --udp-socket-buffer-bytes) UDP_SOCKET_BUFFER_BYTES="$2"; shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

MOQTEST_SERVER="$MOQBIN/moqtest_server"
MOQPERF_CLIENT="$MOQBIN/moqperf_test_client"
METRICS_SCRIPT="$REPO/scripts/perf-metrics.sh"

# ── jemalloc resolution ───────────────────────────────────────────────────────
if [[ "$JEMALLOC" == "auto" ]]; then
  if [[ -f /lib64/libjemalloc.so.2 ]]; then
    JEMALLOC=/lib64/libjemalloc.so.2
  else
    echo "WARNING: --jemalloc requested but /lib64/libjemalloc.so.2 not found; ignoring" >&2
    JEMALLOC=""
  fi
fi

# ── Log directory (always on) ─────────────────────────────────────────────────
LOG_DIR="/tmp/moqx-perf-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$LOG_DIR"
RELAY_LOG="$LOG_DIR/relay.log"
SERVER_LOG="$LOG_DIR/server.log"
CLIENT_LOG="$LOG_DIR/client.log"
echo "Logs: $LOG_DIR"

# ── Prereq checks ──────────────────────────────────────────────────────────────
CHECK_BINS=("$BINARY" "$MOQTEST_SERVER")
[[ -z "$REMOTE_CLIENT_HOST" ]] && CHECK_BINS+=("$MOQPERF_CLIENT")
[[ "$RUN_METRICS" == true ]] && CHECK_BINS+=("$METRICS_SCRIPT")
if [[ "$PERF_DURATION" -gt 0 ]] && ! command -v perf >/dev/null 2>&1; then
  echo "ERROR: --perf-duration requires 'perf' in PATH" >&2; exit 1
fi
for f in "${CHECK_BINS[@]}"; do
  if [[ ! -x "$f" ]]; then
    echo "ERROR: not found or not executable: $f" >&2; exit 1
  fi
done

if [[ "$TRANSPORT" != "quic" && "$TRANSPORT" != "webtransport" ]]; then
  echo "ERROR: --transport must be 'quic' or 'webtransport'" >&2; exit 1
fi
if [[ "$RAMP" -le 0 ]]; then
  echo "ERROR: --ramp must be > 0" >&2; exit 1
fi

is_port_in_use() {
  if command -v ss >/dev/null 2>&1; then
    ss -tunlp 2>/dev/null | grep -q ":$1 "
  else
    lsof -iTCP:"$1" -sTCP:LISTEN -P -n 2>/dev/null | grep -q .
  fi
}
for port in "$RELAY_PORT" "$RELAY_ADMIN_PORT"; do
  if is_port_in_use "$port"; then
    echo "ERROR: port $port already in use" >&2; exit 1
  fi
done

# ── URL scheme ────────────────────────────────────────────────────────────────
if [[ -n "$REMOTE_CLIENT_HOST" ]]; then
  LOCAL_IP="$(hostname -I | awk '{print $1}')"
  RELAY_URL="https://${LOCAL_IP}:${RELAY_PORT}${ENDPOINT}"
else
  RELAY_URL="https://127.0.0.1:${RELAY_PORT}${ENDPOINT}"
fi
if [[ "$TRANSPORT" == "quic" ]]; then
  QUIC_FLAG="--quic_transport=true"
else
  QUIC_FLAG="--quic_transport=false"
fi

# ── Temp dir for relay config ─────────────────────────────────────────────────
TMPDIR_SCRIPT="$(mktemp -d)"
RELAY_CFG="$TMPDIR_SCRIPT/relay.yaml"

# ── Cleanup ───────────────────────────────────────────────────────────────────
PIDS=()
RELAY_PID=""
PERF_STAT_PID=""
cleanup() {
  # Send SIGINT to perf stat first so it prints its summary before we kill the relay
  [[ -n "$PERF_STAT_PID" ]] && kill -INT "$PERF_STAT_PID" 2>/dev/null || true
  [[ -n "$RELAY_PID" ]] && kill "$RELAY_PID" 2>/dev/null || true
  # Wait for perf stat to finish writing (up to 3s)
  if [[ -n "$PERF_STAT_PID" ]]; then
    local ps_deadline=$(( $(date +%s) + 3 ))
    while kill -0 "$PERF_STAT_PID" 2>/dev/null; do
      (( $(date +%s) >= ps_deadline )) && { kill -KILL "$PERF_STAT_PID" 2>/dev/null || true; break; }
      sleep 0.1
    done
    wait "$PERF_STAT_PID" 2>/dev/null || true
  fi
  for pid in ${PIDS[@]+"${PIDS[@]}"}; do kill "$pid" 2>/dev/null || true; done
  local deadline=$(( $(date +%s) + 5 ))
  for pid in ${PIDS[@]+"${PIDS[@]}"}; do
    while kill -0 "$pid" 2>/dev/null; do
      (( $(date +%s) >= deadline )) && { kill -KILL "$pid" 2>/dev/null || true; break; }
      sleep 0.1
    done
  done
  [[ ${#PIDS[@]} -gt 0 ]] && wait ${PIDS[@]+"${PIDS[@]}"} 2>/dev/null || true
  [[ -n "$RELAY_PID" ]] && wait "$RELAY_PID" 2>/dev/null || true
  rm -rf "$TMPDIR_SCRIPT"
  echo "Logs saved to $LOG_DIR"
}
trap cleanup EXIT

# ── Relay config ──────────────────────────────────────────────────────────────
cat >"$RELAY_CFG" <<EOF
relay_id: "perf-test-relay"
threads: $IO_THREADS
use_relay_thread: $USE_RELAY_THREAD
mvfst_bpf_steering: $BPF_STEERING
listeners:
  - name: perf
    udp:
      socket:
        address: "::"
        port: $RELAY_PORT
    tls:
      insecure: true
    endpoint: "$ENDPOINT"
    mvfst:
      enable_gso: true
      max_conn_packets_sent_per_loop: 16
      max_server_recv_packets_per_loop: 256
      udp_socket_buffer_bytes: $UDP_SOCKET_BUFFER_BYTES
      bbr2:
        exit_startup_on_loss: true
        enable_recovery_in_startup: true
        enable_recovery_in_probe_states: true
    quic:
      cc_algo: bbr2
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache:
      enabled: false
      max_tracks: 0
      max_groups_per_track: 0
admin:
  port: $RELAY_ADMIN_PORT
  address: "::"
  plaintext: true
EOF

cp "$RELAY_CFG" "$LOG_DIR/relay.yaml"

# ── Run params ────────────────────────────────────────────────────────────────
{
  echo "date:             $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "moqx_git:         $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "moxygen_git:      $(git -C "$REPO/deps/moxygen" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "relay_binary:     $BINARY"
  echo "moqbin:           $MOQBIN"
  echo "relay_url:        $RELAY_URL"
  echo "transport:        $TRANSPORT"
  echo "io_threads:       $IO_THREADS"
  echo "use_relay_thread: $USE_RELAY_THREAD"
  echo "subscriber_max:   $SUBSCRIBER_MAX"
  echo "ramp:             $RAMP"
  echo "duration:         $DURATION"
  echo "delivery_timeout: $DELIVERY_TIMEOUT"
  echo "client_threads:   $CLIENT_THREADS"
  echo "jemalloc:         ${JEMALLOC:-none}"
  echo "udp_socket_buf:     $UDP_SOCKET_BUFFER_BYTES"
  echo "mvfst_bpf_steering: $BPF_STEERING"
  [[ "$PERF_DURATION" -gt 0 ]] && echo "perf_duration:    ${PERF_DURATION}s (delay=$(( 3 * SUBSCRIBER_MAX / RAMP ))s)" || true
  [[ -n "$PERF_EVENTS" ]] && echo "perf_events:      $PERF_EVENTS" || true
  [[ -n "$REMOTE_CLIENT_HOST" ]] && echo "remote_client:    $REMOTE_CLIENT_HOST" || true
} | tee "$LOG_DIR/run_params.txt"
echo ""

# ── Start relay ───────────────────────────────────────────────────────────────
ulimit -n 65536 2>/dev/null || true
echo "Starting relay (use_relay_thread=$USE_RELAY_THREAD, io_threads=$IO_THREADS, transport=$TRANSPORT, mvfst_bpf_steering=$BPF_STEERING)..."
RELAY_LOGGING_ARG=()
[[ -n "$RELAY_LOG_SPEC" ]] && RELAY_LOGGING_ARG=("--logging=$RELAY_LOG_SPEC")
if [[ -n "$JEMALLOC" ]]; then
  echo "Using jemalloc: $JEMALLOC"
  LD_PRELOAD="$JEMALLOC" "$BINARY" --config="$RELAY_CFG" ${RELAY_LOGGING_ARG[@]+"${RELAY_LOGGING_ARG[@]}"} >"$RELAY_LOG" 2>&1 &
else
  "$BINARY" --config="$RELAY_CFG" ${RELAY_LOGGING_ARG[@]+"${RELAY_LOGGING_ARG[@]}"} >"$RELAY_LOG" 2>&1 &
fi
RELAY_PID=$!

deadline=$(( $(date +%s) + 10 ))
until curl -sf "http://localhost:$RELAY_ADMIN_PORT/info" >/dev/null 2>&1; do
  (( $(date +%s) >= deadline )) && { echo "ERROR: relay not ready after 10s" >&2; exit 1; }
  sleep 0.1
done
echo "Relay ready on port $RELAY_PORT"

# ── Trace script (optional) ───────────────────────────────────────────────────
if [[ -n "$TRACE_SCRIPT" ]]; then
  echo "Starting trace script $TRACE_SCRIPT (pid=$RELAY_PID) → $LOG_DIR/trace.log"
  bash "$TRACE_SCRIPT" "$RELAY_PID" >"$LOG_DIR/trace.log" 2>&1 &
  PIDS+=($!)
fi

# ── perf stat (optional) ──────────────────────────────────────────────────────
if [[ "$RUN_PERF_STAT" == true ]]; then
  perf_stat_out="$LOG_DIR/perf-stat.txt"
  echo "Starting perf stat → $perf_stat_out"
  perf stat -e cycles,instructions,cache-misses,cache-references,context-switches,cpu-migrations,dTLB-load-misses,L1-dcache-load-misses \
    -p "$RELAY_PID" >"$perf_stat_out" 2>&1 &
  PERF_STAT_PID=$!
fi

# ── Start metrics poller (optional) ──────────────────────────────────────────
if [[ "$RUN_METRICS" == true ]]; then
  METRICS_LOG="$LOG_DIR/metrics.log"
  echo "Starting metrics poller → $METRICS_LOG"
  bash "$METRICS_SCRIPT" "$RELAY_ADMIN_PORT" "$METRICS_LOG" >"$LOG_DIR/metrics_stderr.log" 2>&1 &
  PIDS+=($!)
fi

# ── Start moqtest_server (publisher) ─────────────────────────────────────────
echo "Starting moqtest_server -> $RELAY_URL ..."
"$MOQTEST_SERVER" \
  --relay_url="$RELAY_URL" \
  $QUIC_FLAG \
  --include_timestamp_extension=true \
  >"$SERVER_LOG" 2>&1 &
PIDS+=($!)

deadline=$(( $(date +%s) + 10 ))
until [[ "$(curl -sf "http://localhost:$RELAY_ADMIN_PORT/metrics" 2>/dev/null \
           | grep "^moqx_moqActiveSessions " | awk '{print $2}')" -ge 1 ]] 2>/dev/null; do
  (( $(date +%s) >= deadline )) && { echo "ERROR: moqtest_server did not connect after 10s" >&2; exit 1; }
  sleep 0.1
done
echo "Publisher connected"

# ── Start perf record (optional) ─────────────────────────────────────────────
if [[ "$PERF_DURATION" -gt 0 ]]; then
  perf_delay=$(( 3 * SUBSCRIBER_MAX / RAMP ))
  perf_data="$LOG_DIR/perf.data"
  echo "perf record: starts in ${perf_delay}s, runs ${PERF_DURATION}s → $perf_data"
  ( sleep "$perf_delay" && \
    perf record -F 499 -g -e cycles -p "$RELAY_PID" -o "$perf_data" -- sleep "$PERF_DURATION" \
    && echo "perf record complete → $perf_data" ) &
  PIDS+=($!)
fi

if [[ -n "$PERF_EVENTS" && "$PERF_DURATION" -gt 0 ]]; then
  perf_delay=$(( 3 * SUBSCRIBER_MAX / RAMP ))
  perf_events_data="$LOG_DIR/perf-events.data"
  echo "perf record ($PERF_EVENTS): starts in ${perf_delay}s, runs ${PERF_DURATION}s → $perf_events_data"
  ( sleep "$perf_delay" && \
    perf record -F 99 -g -e "$PERF_EVENTS" -p "$RELAY_PID" -o "$perf_events_data" -- sleep "$PERF_DURATION" \
    && echo "perf record ($PERF_EVENTS) complete → $perf_events_data" ) &
  PIDS+=($!)
elif [[ -n "$PERF_EVENTS" ]]; then
  echo "WARNING: --perf-events requires --perf-duration; ignoring" >&2
fi

# ── Run moqperf_test_client ───────────────────────────────────────────────────
CLIENT_ARGS=(
  --relay_url="$RELAY_URL"
  $QUIC_FLAG
  --subscriber_max="$SUBSCRIBER_MAX"
  --subscriber_ramp="$RAMP"
  --duration="$DURATION"
  --delivery_timeout="$DELIVERY_TIMEOUT"
  --num_threads="$CLIENT_THREADS"
  "${CLIENT_EXTRA_ARGS[@]+"${CLIENT_EXTRA_ARGS[@]}"}"
)

if [[ -n "$REMOTE_CLIENT_HOST" ]]; then
  echo "Syncing moqperf_test_client to $REMOTE_CLIENT_HOST..."
  rsync -az -e ssh "$MOQPERF_CLIENT" "${REMOTE_CLIENT_HOST}:/tmp/moqperf_test_client"
  echo "Running perf client on $REMOTE_CLIENT_HOST: subscriber_max=$SUBSCRIBER_MAX ramp=$RAMP duration=${DURATION}s delivery_timeout=${DELIVERY_TIMEOUT}ms threads=$CLIENT_THREADS"
  echo "---"
  ssh "$REMOTE_CLIENT_HOST" \
    "ulimit -n 65536 2>/dev/null || true; /tmp/moqperf_test_client ${CLIENT_ARGS[*]@Q}" \
    2>&1 | tee "$CLIENT_LOG"
else
  ulimit -n 65536 2>/dev/null || true
  echo "Running perf client: subscriber_max=$SUBSCRIBER_MAX ramp=$RAMP duration=${DURATION}s delivery_timeout=${DELIVERY_TIMEOUT}ms threads=$CLIENT_THREADS"
  echo "---"
  "$MOQPERF_CLIENT" "${CLIENT_ARGS[@]}" 2>&1 | tee "$CLIENT_LOG"
fi
