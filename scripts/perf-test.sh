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
#   --local-forwarders     Enable per-subscriber-thread local forwarders
#                          (use_local_forwarders: true; requires relay thread)
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
#                          Repeat the flag to use multiple client machines;
#                          subscriber load (--subscriber-max, --ramp) is
#                          divided evenly among them.  Each machine writes
#                          its own client-<host>.log in LOG_DIR.
#   --udp-socket-buffer-bytes N
#                          UDP socket send/recv buffer size in bytes for the
#                          relay listener (defaults to net.core.wmem_max).
#   --relay-url URL        Connect to an existing remote relay instead of
#                          starting one locally.  Publisher (moqtest_server)
#                          runs locally; subscriber runs via --remote-client.
#                          Example: --relay-url https://relay.example.com:4433/moq-relay
#   --relay-admin-url URL  Admin endpoint of the remote relay for metrics
#                          collection and publisher-connected checks.
#                          Required when --relay-url is set.
#                          Example: --relay-admin-url http://relay.example.com:8000
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
USE_LOCAL_FORWARDERS="false"
IO_THREADS=1
CLIENT_THREADS=2
RELAY_LOG_SPEC=""
BPF_STEERING="false"
JEMALLOC=""
RUN_METRICS=false
PERF_DURATION=0
PERF_EVENTS=""
RUN_PERF_STAT=false
REMOTE_CLIENT_HOSTS=()
REMOTE_RELAY_URL=""
REMOTE_RELAY_ADMIN_URL=""
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
    --local-forwarders) USE_LOCAL_FORWARDERS="true"; shift ;;
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
    --remote-client)    REMOTE_CLIENT_HOSTS+=("$2"); shift 2 ;;
    --relay-url)        REMOTE_RELAY_URL="$2";    shift 2 ;;
    --relay-admin-url)  REMOTE_RELAY_ADMIN_URL="$2"; shift 2 ;;
    --udp-socket-buffer-bytes) UDP_SOCKET_BUFFER_BYTES="$2"; shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

MOQTEST_SERVER="$MOQBIN/moqtest_server"
MOQPERF_CLIENT="$MOQBIN/moqperf_test_client"
METRICS_SCRIPT="$REPO/scripts/perf-metrics.sh"
COLLECT_LIBS_SCRIPT="$REPO/scripts/collect-libs.sh"

# ── Remote relay mode ─────────────────────────────────────────────────────────
REMOTE_RELAY=false
if [[ -n "$REMOTE_RELAY_URL" ]]; then
  REMOTE_RELAY=true
  if [[ -z "$REMOTE_RELAY_ADMIN_URL" ]]; then
    echo "ERROR: --relay-admin-url is required when --relay-url is set" >&2; exit 1
  fi
  RELAY_URL="$REMOTE_RELAY_URL"
  RELAY_ADMIN_URL="${REMOTE_RELAY_ADMIN_URL%/}"
  echo "Remote relay mode: relay=$RELAY_URL admin=$RELAY_ADMIN_URL"
fi

# ── jemalloc resolution ───────────────────────────────────────────────────────
if [[ "$REMOTE_RELAY" == true && "$JEMALLOC" == "auto" ]]; then
  echo "WARNING: --jemalloc ignored in remote relay mode" >&2
  JEMALLOC=""
elif [[ "$JEMALLOC" == "auto" ]]; then
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
if [[ "$REMOTE_RELAY" == true ]]; then
  CHECK_BINS=("$MOQTEST_SERVER")
else
  CHECK_BINS=("$BINARY" "$MOQTEST_SERVER")
fi
[[ ${#REMOTE_CLIENT_HOSTS[@]} -eq 0 ]] && CHECK_BINS+=("$MOQPERF_CLIENT")
[[ "$RUN_METRICS" == true ]] && CHECK_BINS+=("$METRICS_SCRIPT")
if [[ "$PERF_DURATION" -gt 0 && "$REMOTE_RELAY" == false ]] && ! command -v perf >/dev/null 2>&1; then
  echo "ERROR: --perf-duration requires 'perf' in PATH" >&2; exit 1
fi
if [[ ${#REMOTE_CLIENT_HOSTS[@]} -gt 0 ]]; then
  if [[ ! -f "$COLLECT_LIBS_SCRIPT" ]]; then
    echo "ERROR: required helper script not found: $COLLECT_LIBS_SCRIPT" >&2; exit 1
  fi
  if ! command -v ldd >/dev/null 2>&1; then
    echo "ERROR: --remote-client requires 'ldd' for auto library collection" >&2; exit 1
  fi
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

get_metric_value() {
  local metric_name="$1"
  curl -sf "$RELAY_ADMIN_URL/metrics" 2>/dev/null \
    | awk -v m="$metric_name" '$1 == m { print $2; found=1; exit } END { if (!found) print "" }'
}

if [[ "$REMOTE_RELAY" == false ]]; then
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
fi

# ── URL scheme ────────────────────────────────────────────────────────────────
if [[ "$REMOTE_RELAY" == false ]]; then
  if [[ ${#REMOTE_CLIENT_HOSTS[@]} -gt 0 ]]; then
    LOCAL_IP="$(hostname -I | awk '{print $1}')"
    RELAY_URL="https://${LOCAL_IP}:${RELAY_PORT}${ENDPOINT}"
  else
    RELAY_URL="https://127.0.0.1:${RELAY_PORT}${ENDPOINT}"
  fi
  RELAY_ADMIN_URL="http://localhost:${RELAY_ADMIN_PORT}"
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
if [[ "$REMOTE_RELAY" == false ]]; then
cat >"$RELAY_CFG" <<EOF
relay_id: "perf-test-relay"
threads: $IO_THREADS
use_relay_thread: $USE_RELAY_THREAD
use_local_forwarders: $USE_LOCAL_FORWARDERS
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
      ignore_path_mtu: true
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
fi

# ── Run params ────────────────────────────────────────────────────────────────
{
  echo "date:             $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "moqx_git:         $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "moxygen_git:      $(git -C "$REPO/deps/moxygen" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "mode:             $(if [[ "$REMOTE_RELAY" == true ]]; then echo "remote-relay"; else echo "local"; fi)"
  [[ "$REMOTE_RELAY" == false ]] && echo "relay_binary:     $BINARY"
  echo "moqbin:           $MOQBIN"
  echo "relay_url:        $RELAY_URL"
  [[ "$REMOTE_RELAY" == true ]] && echo "relay_admin_url:  $RELAY_ADMIN_URL"
  echo "transport:        $TRANSPORT"
  echo "io_threads:       $IO_THREADS"
  echo "use_relay_thread: $USE_RELAY_THREAD"
  echo "local_forwarders: $USE_LOCAL_FORWARDERS"
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
  [[ ${#REMOTE_CLIENT_HOSTS[@]} -gt 0 ]] && echo "remote_clients:   ${REMOTE_CLIENT_HOSTS[*]}" || true
} | tee "$LOG_DIR/run_params.txt"
echo ""

# ── Start relay ───────────────────────────────────────────────────────────────
ulimit -n 65536 2>/dev/null || true
if [[ "$REMOTE_RELAY" == false ]]; then
  echo "Starting relay (use_relay_thread=$USE_RELAY_THREAD, local_forwarders=$USE_LOCAL_FORWARDERS, io_threads=$IO_THREADS, transport=$TRANSPORT, mvfst_bpf_steering=$BPF_STEERING)..."
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
else
  echo "Checking remote relay at $RELAY_ADMIN_URL ..."
  deadline=$(( $(date +%s) + 10 ))
  until curl -sf "$RELAY_ADMIN_URL/info" >/dev/null 2>&1; do
    (( $(date +%s) >= deadline )) && { echo "ERROR: remote relay not reachable at $RELAY_ADMIN_URL/info after 10s" >&2; exit 1; }
    sleep 0.1
  done
  echo "Remote relay reachable"
fi

# ── Trace script (optional) ───────────────────────────────────────────────────
if [[ -n "$TRACE_SCRIPT" ]]; then
  if [[ "$REMOTE_RELAY" == true ]]; then
    echo "WARNING: --trace-script ignored in remote relay mode (no local relay PID)" >&2
  else
    echo "Starting trace script $TRACE_SCRIPT (pid=$RELAY_PID) → $LOG_DIR/trace.log"
    bash "$TRACE_SCRIPT" "$RELAY_PID" >"$LOG_DIR/trace.log" 2>&1 &
    PIDS+=($!)
  fi
fi

# ── perf stat (optional) ──────────────────────────────────────────────────────
if [[ "$RUN_PERF_STAT" == true ]]; then
  if [[ "$REMOTE_RELAY" == true ]]; then
    echo "WARNING: --perf-stat ignored in remote relay mode (no local relay PID)" >&2
  else
    perf_stat_out="$LOG_DIR/perf-stat.txt"
    echo "Starting perf stat → $perf_stat_out"
    perf stat -e cycles,instructions,cache-misses,cache-references,context-switches,cpu-migrations,dTLB-load-misses,L1-dcache-load-misses \
      -p "$RELAY_PID" >"$perf_stat_out" 2>&1 &
    PERF_STAT_PID=$!
  fi
fi

# ── Start metrics poller (optional) ──────────────────────────────────────────
if [[ "$RUN_METRICS" == true ]]; then
  METRICS_LOG="$LOG_DIR/metrics.log"
  echo "Starting metrics poller → $METRICS_LOG"
  bash "$METRICS_SCRIPT" "$RELAY_ADMIN_URL" "$METRICS_LOG" >"$LOG_DIR/metrics_stderr.log" 2>&1 &
  PIDS+=($!)
fi

# ── Snapshot metrics before publisher starts (baseline for publisher-connected check) ──
baseline_sessions="$(get_metric_value "moqx_moqActiveSessions")" || true
baseline_sessions="${baseline_sessions:-0}"
baseline_publishers="$(get_metric_value "moqx_pubActivePublishers")" || true
have_publisher_metric=false
if [[ -n "$baseline_publishers" ]]; then
  have_publisher_metric=true
else
  baseline_publishers=0
fi

# ── Start moqtest_server (publisher) ─────────────────────────────────────────
echo "Starting moqtest_server -> $RELAY_URL ..."
"$MOQTEST_SERVER" \
  --relay_url="$RELAY_URL" \
  $QUIC_FLAG \
  --include_timestamp_extension=true \
  >"$SERVER_LOG" 2>&1 &
PIDS+=($!)

deadline=$(( $(date +%s) + 30 ))
until false; do
  current_sessions="$(get_metric_value "moqx_moqActiveSessions")" || true
  current_sessions="${current_sessions:-0}"

  if [[ "$have_publisher_metric" == true ]]; then
    current_publishers="$(get_metric_value "moqx_pubActivePublishers")" || true
    current_publishers="${current_publishers:-0}"
    if [[ "$current_publishers" -ge $(( baseline_publishers + 1 )) ]]; then
      break
    fi
  elif [[ "$current_sessions" -ge $(( baseline_sessions + 1 )) ]]; then
    break
  fi

  if (( $(date +%s) >= deadline )); then
    echo "ERROR: moqtest_server did not connect after 30s" >&2
    echo "DEBUG: baseline_sessions=$baseline_sessions current_sessions=$current_sessions" >&2
    if [[ "$have_publisher_metric" == true ]]; then
      echo "DEBUG: baseline_publishers=$baseline_publishers current_publishers=${current_publishers:-0}" >&2
    fi
    curl -sf "$RELAY_ADMIN_URL/metrics" 2>/dev/null \
      | grep -E '^moqx_(moqActiveSessions|pubActivePublishers|pubPublishError_total|subSubscribeError_total) ' >&2 || true
    tail -40 "$SERVER_LOG" >&2 || true
    exit 1
  fi

  sleep 0.25
done
echo "Publisher connected"

# ── Start perf record (optional) ─────────────────────────────────────────────
if [[ "$PERF_DURATION" -gt 0 ]]; then
  if [[ "$REMOTE_RELAY" == true ]]; then
    echo "WARNING: --perf-duration ignored in remote relay mode (no local relay PID)" >&2
  else
    perf_delay=$(( 3 * SUBSCRIBER_MAX / RAMP ))
    perf_data="$LOG_DIR/perf.data"
    echo "perf record: starts in ${perf_delay}s, runs ${PERF_DURATION}s → $perf_data"
    ( sleep "$perf_delay" && \
      perf record -F 499 -g -e cycles -p "$RELAY_PID" -o "$perf_data" -- sleep "$PERF_DURATION" \
      && echo "perf record complete → $perf_data" ) &
    PIDS+=($!)
  fi
fi

if [[ -n "$PERF_EVENTS" && "$PERF_DURATION" -gt 0 && "$REMOTE_RELAY" == false ]]; then
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

if [[ ${#REMOTE_CLIENT_HOSTS[@]} -gt 0 ]]; then
  NUM_CLIENTS=${#REMOTE_CLIENT_HOSTS[@]}
  REMOTE_CLIENT_LIBDIR_LOCAL="$TMPDIR_SCRIPT/remote-client-lib"
  REMOTE_CLIENT_LIBDIR_REMOTE="/tmp/moqx-perf-lib-${LOG_DIR##*/}"

  echo "Collecting shared libraries for remote client binary..."
  mkdir -p "$REMOTE_CLIENT_LIBDIR_LOCAL"
  bash "$COLLECT_LIBS_SCRIPT" "$MOQPERF_CLIENT" "$REMOTE_CLIENT_LIBDIR_LOCAL" > /dev/null
  REMOTE_CLIENT_LIB_COUNT=$(find "$REMOTE_CLIENT_LIBDIR_LOCAL" -maxdepth 1 -type f -name '*.so*' | wc -l | tr -d '[:space:]')
  echo "Collected $REMOTE_CLIENT_LIB_COUNT shared library file(s); syncing to each remote client"

  # Helper function: rsync with retry logic (up to 3 attempts with exponential backoff)
  rsync_with_retry() {
    local src="$1"
    local dest="$2"
    local max_attempts=3
    local attempt=1
    while (( attempt <= max_attempts )); do
      if rsync -az -e ssh "$src" "$dest"; then
        return 0
      fi
      if (( attempt < max_attempts )); then
        local delay=$(( 2 ** (attempt - 1) ))
        echo "WARNING: rsync failed (attempt $attempt/$max_attempts); retrying in ${delay}s..."
        sleep "$delay"
      fi
      (( attempt++ ))
    done
    echo "ERROR: rsync failed after $max_attempts attempts: src=$src dest=$dest" >&2
    return 1
  }

  # Divide load evenly; last client absorbs any remainder from integer division
  PER_CLIENT_MAX=$(( SUBSCRIBER_MAX / NUM_CLIENTS ))
  PER_CLIENT_RAMP=$(( RAMP / NUM_CLIENTS ))
  [[ $PER_CLIENT_RAMP -lt 1 ]] && PER_CLIENT_RAMP=1

  REMOTE_PIDS=()
  REMOTE_LOGS=()
  REMOTE_HOST_LABELS=()
  REMOTE_CLIENT_ARGS_ARRAY=()

  # ── Phase 1: Transfer files to all remote clients (sequentially, with retries) ──
  echo "Syncing binaries and libraries to all remote clients..."
  for i in "${!REMOTE_CLIENT_HOSTS[@]}"; do
    host="${REMOTE_CLIENT_HOSTS[$i]}"
    host_label="${host##*@}"
    host_label="${host_label//[^a-zA-Z0-9._-]/_}"
    REMOTE_HOST_LABELS+=("$host_label")

    echo "  [$((i + 1))/$NUM_CLIENTS] $host: creating directories..."
    ssh "$host" "mkdir -p '$REMOTE_CLIENT_LIBDIR_REMOTE'" || {
      echo "ERROR: failed to create remote directory on $host" >&2
      exit 1
    }

    echo "  [$((i + 1))/$NUM_CLIENTS] $host: syncing moqperf_test_client..."
    rsync_with_retry "$MOQPERF_CLIENT" "${host}:/tmp/moqperf_test_client" || exit 1

    echo "  [$((i + 1))/$NUM_CLIENTS] $host: syncing libraries..."
    rsync_with_retry "$REMOTE_CLIENT_LIBDIR_LOCAL/" "${host}:${REMOTE_CLIENT_LIBDIR_REMOTE}/" || exit 1
  done
  echo "All files transferred successfully"

  # ── Phase 2: Launch all remote clients (roughly simultaneously) ──
  for i in "${!REMOTE_CLIENT_HOSTS[@]}"; do
    host="${REMOTE_CLIENT_HOSTS[$i]}"
    host_label="${REMOTE_HOST_LABELS[$i]}"
    client_log="$LOG_DIR/client-${host_label}.log"
    REMOTE_LOGS+=("$client_log")

    # Last client absorbs remainder so totals match the requested values
    if [[ $i -eq $(( NUM_CLIENTS - 1 )) ]]; then
      this_max=$(( SUBSCRIBER_MAX - PER_CLIENT_MAX * (NUM_CLIENTS - 1) ))
      this_ramp=$(( RAMP - PER_CLIENT_RAMP * (NUM_CLIENTS - 1) ))
      [[ $this_ramp -lt 1 ]] && this_ramp=1
    else
      this_max=$PER_CLIENT_MAX
      this_ramp=$PER_CLIENT_RAMP
    fi

    THIS_CLIENT_ARGS=(
      --relay_url="$RELAY_URL"
      $QUIC_FLAG
      --subscriber_max="$this_max"
      --subscriber_ramp="$this_ramp"
      --duration="$DURATION"
      --delivery_timeout="$DELIVERY_TIMEOUT"
      --num_threads="$CLIENT_THREADS"
      "${CLIENT_EXTRA_ARGS[@]+"${CLIENT_EXTRA_ARGS[@]}"}"
    )
    REMOTE_CLIENT_ARGS_ARRAY+=("${THIS_CLIENT_ARGS[@]}")

    echo "Starting perf client on $host: subscriber_max=$this_max ramp=$this_ramp duration=${DURATION}s delivery_timeout=${DELIVERY_TIMEOUT}ms threads=$CLIENT_THREADS → $client_log"
    if [[ $NUM_CLIENTS -eq 1 ]]; then
      # Single remote client: stream output directly (no prefix needed)
      ( ssh "$host" \
      "ulimit -n 65536 2>/dev/null || true; env LD_LIBRARY_PATH='${REMOTE_CLIENT_LIBDIR_REMOTE}':\${LD_LIBRARY_PATH:-} /tmp/moqperf_test_client ${THIS_CLIENT_ARGS[*]@Q}" \
          2>&1 | tee "$client_log" ) &
    else
      # Multiple clients: prefix each output line with [host] so streams are distinguishable
      ( ssh "$host" \
      "ulimit -n 65536 2>/dev/null || true; env LD_LIBRARY_PATH='${REMOTE_CLIENT_LIBDIR_REMOTE}':\${LD_LIBRARY_PATH:-} /tmp/moqperf_test_client ${THIS_CLIENT_ARGS[*]@Q}" \
          2>&1 | tee "$client_log" | sed "s/^/[$host_label] /" ) &
    fi
    REMOTE_PIDS+=($!)
  done

  echo "---"
  echo "Waiting for $NUM_CLIENTS remote client(s) (total subscriber_max=$SUBSCRIBER_MAX ramp=$RAMP)..."
  all_ok=true
  for i in "${!REMOTE_PIDS[@]}"; do
    wait "${REMOTE_PIDS[$i]}" || { echo "WARNING: client ${REMOTE_CLIENT_HOSTS[$i]} exited non-zero" >&2; all_ok=false; }
  done
  echo ""
  echo "Per-client logs: ${REMOTE_LOGS[*]}"
else
  ulimit -n 65536 2>/dev/null || true
  echo "Running perf client: subscriber_max=$SUBSCRIBER_MAX ramp=$RAMP duration=${DURATION}s delivery_timeout=${DELIVERY_TIMEOUT}ms threads=$CLIENT_THREADS"
  echo "---"
  "$MOQPERF_CLIENT" "${CLIENT_ARGS[@]}" 2>&1 | tee "$CLIENT_LOG"
fi
