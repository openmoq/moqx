#!/usr/bin/env bash
# perf-results-to-json.sh — Parse moqperf_test_client output and metrics
# into a structured JSON results file for trend tracking.
#
# Usage: scripts/perf-results-to-json.sh [options]
#   --client-output PATH   Path to client stdout capture
#   --metrics-log PATH     Path to metrics TSV log (optional)
#   --commit SHA           Git commit SHA
#   --branch NAME          Git branch name
#   --timestamp ISO        ISO timestamp
#   --subscriber-max N     Test parameter
#   --ramp N               Test parameter
#   --duration N           Test parameter
#   --io-threads N         Test parameter
#   --client-threads N     Test parameter
#   --client-window-start N  Optional [AGGREGATE] second to start latency averaging
#   --client-window-end N    Optional [AGGREGATE] second to end latency averaging
#   --relay-cpu PCT        Average relay CPU%
#   --relay-rss-kb N       Peak relay RSS in KB
#   --net-throughput-mbps N  Network throughput in Mbps
#   --output PATH          Output JSON file

set -euo pipefail

# ── Defaults ───────────────────────────────────────────────────────────────────
CLIENT_OUTPUT=""
METRICS_LOG=""
COMMIT=""
BRANCH=""
TIMESTAMP=""
SUBSCRIBER_MAX=0
RAMP=0
DURATION=0
IO_THREADS=0
CLIENT_THREADS=0
CLIENT_WINDOW_START=""
CLIENT_WINDOW_END=""
RELAY_CPU="0"
RELAY_RSS_KB="0"
NET_THROUGHPUT_MBPS="0"
DELIVERY_TIMEOUT=500
TRANSPORT="quic"
OUTPUT="perf-results.json"

# ── Arg parsing ────────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --client-output)       CLIENT_OUTPUT="$2";       shift 2 ;;
    --metrics-log)         METRICS_LOG="$2";         shift 2 ;;
    --commit)              COMMIT="$2";              shift 2 ;;
    --branch)              BRANCH="$2";              shift 2 ;;
    --timestamp)           TIMESTAMP="$2";           shift 2 ;;
    --subscriber-max)      SUBSCRIBER_MAX="$2";      shift 2 ;;
    --ramp)                RAMP="$2";                shift 2 ;;
    --duration)            DURATION="$2";            shift 2 ;;
    --io-threads)          IO_THREADS="$2";          shift 2 ;;
    --client-threads)      CLIENT_THREADS="$2";      shift 2 ;;
    --client-window-start) CLIENT_WINDOW_START="$2"; shift 2 ;;
    --client-window-end)   CLIENT_WINDOW_END="$2";   shift 2 ;;
    --relay-cpu)           RELAY_CPU="$2";           shift 2 ;;
    --relay-rss-kb)        RELAY_RSS_KB="$2";        shift 2 ;;
    --net-throughput-mbps) NET_THROUGHPUT_MBPS="$2"; shift 2 ;;
    --delivery-timeout)    DELIVERY_TIMEOUT="$2";    shift 2 ;;
    --transport)           TRANSPORT="$2";           shift 2 ;;
    --output)              OUTPUT="$2";              shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

if [[ -z "$CLIENT_OUTPUT" || ! -f "$CLIENT_OUTPUT" ]]; then
  echo "ERROR: --client-output file required and must exist" >&2; exit 1
fi

# ── Parse client output ───────────────────────────────────────────────────────
# Extract final summary values from moqperf_test_client output.
# Lines like: "  Total Subscribers: 1000"
#             "  Total Objects: 45000"
#             "  Total Bytes: 123456789"
#             "  Total Resets: 0"
#             "  Duration: 120 seconds"
#             "  Throughput: 85.5 Mbps"
#             "  Average Latency: 12.3 ms"
#             "  Result: SUCCESS - Track ended naturally"

parse_field() {
  local pattern="$1"
  local file="$2"
  grep -oP "$pattern" "$file" | tail -1 || echo "0"
}

TOTAL_SUBSCRIBERS=$(grep "Total Subscribers:" "$CLIENT_OUTPUT" | tail -1 | awk '{print $NF}' || echo "0")
TOTAL_OBJECTS=$(grep "Total Objects:" "$CLIENT_OUTPUT" | tail -1 | awk '{print $NF}' || echo "0")
TOTAL_BYTES=$(grep "Total Bytes:" "$CLIENT_OUTPUT" | tail -1 | awk '{print $NF}' || echo "0")
TOTAL_RESETS=$(grep "Total Resets:" "$CLIENT_OUTPUT" | tail -1 | awk '{print $NF}' || echo "0")
CLIENT_DURATION=$(grep "Duration:" "$CLIENT_OUTPUT" | tail -1 | awk '{print $(NF-1)}' || echo "$DURATION")
THROUGHPUT_MBPS=$(grep "Throughput:" "$CLIENT_OUTPUT" | tail -1 | awk '{print $(NF-1)}' || echo "0")
TEST_RESULT=$(grep "Result:" "$CLIENT_OUTPUT" | tail -1 | sed 's/.*Result: //' || echo "UNKNOWN")

# Average latency (ms): prefer Latency(run avg) from [AGGREGATE] lines, and if
# a client window is supplied, only include aggregate seconds within that window.
# Example parsed token: "Latency(run avg): 11.2 ms".
if [[ -n "$CLIENT_WINDOW_START" && -n "$CLIENT_WINDOW_END" ]]; then
  AVG_LATENCY_MS=$(awk -v s="$CLIENT_WINDOW_START" -v e="$CLIENT_WINDOW_END" '
    /\[AGGREGATE\]/ {
      sec=""; lat=""
      if (match($0, /\[([0-9]+)s\]/, t)) sec=t[1]
      if (match($0, /Latency\(run avg\):[[:space:]]*([0-9]+(\.[0-9]+)?)[[:space:]]*ms/, m)) lat=m[1]
      if (sec != "" && lat != "" && sec >= s && sec <= e) { sum += lat; n++ }
    }
    END { if (n > 0) printf "%.2f", sum/n }
  ' "$CLIENT_OUTPUT")
else
  AVG_LATENCY_MS=$(awk '
    /\[AGGREGATE\]/ {
      if (match($0, /Latency\(run avg\):[[:space:]]*([0-9]+(\.[0-9]+)?)[[:space:]]*ms/, m)) { sum += m[1]; n++ }
    }
    END { if (n > 0) printf "%.2f", sum/n }
  ' "$CLIENT_OUTPUT")
fi

# Fallback for older/non-aggregate formats.
if [[ -z "$AVG_LATENCY_MS" ]]; then
  AVG_LATENCY_MS=$(grep -oP '(Average Latency|Avg Latency):\s*\K[0-9]+(?:\.[0-9]+)?' "$CLIENT_OUTPUT" | tail -1 || true)
fi
[[ -z "$AVG_LATENCY_MS" ]] && AVG_LATENCY_MS="0"

# Parse per-second aggregate lines for latency/delivery stats
# Format: [AGGREGATE] [Ns] Subs: N | Obj/s: N | Mbps: N.NN | ...
# Get peak subscribers from aggregate lines
PEAK_SUBS=$(grep '\[AGGREGATE\]' "$CLIENT_OUTPUT" | grep -oP 'Subs: \K[0-9]+' | sort -n | tail -1 || echo "$TOTAL_SUBSCRIBERS")
# Get peak Mbps
PEAK_MBPS=$(grep '\[AGGREGATE\]' "$CLIENT_OUTPUT" | grep -oP 'Mbps: \K[0-9.]+' | sort -n | tail -1 || echo "$THROUGHPUT_MBPS")
# Get total failures
TOTAL_FAILURES=$(grep '\[AGGREGATE\]' "$CLIENT_OUTPUT" | grep -oP 'Failures: [0-9]+/s, \K[0-9]+' | tail -1 || echo "0")

# ── Compute derived metrics ───────────────────────────────────────────────────
# RSS per session (KB)
if [[ "$PEAK_SUBS" -gt 0 && "$RELAY_RSS_KB" -gt 0 ]]; then
  RSS_PER_SESSION=$(awk "BEGIN {printf \"%.1f\", $RELAY_RSS_KB / $PEAK_SUBS}")
else
  RSS_PER_SESSION="0"
fi

# Throughput per CPU% (Mbps per 1% relay CPU)
if awk "BEGIN {exit !($RELAY_CPU > 0)}"; then
  THROUGHPUT_PER_CORE=$(awk "BEGIN {printf \"%.2f\", $THROUGHPUT_MBPS / $RELAY_CPU}")
else
  THROUGHPUT_PER_CORE="0"
fi

# Subscribers per core
if [[ "$IO_THREADS" -gt 0 ]]; then
  SUBS_PER_CORE=$(awk "BEGIN {printf \"%.0f\", $PEAK_SUBS / $IO_THREADS}")
else
  SUBS_PER_CORE="$PEAK_SUBS"
fi

# Delivery success rate (%)
if [[ "$TOTAL_OBJECTS" -gt 0 ]]; then
  # Failures/resets indicate delivery issues
  TOTAL_ISSUES=$((TOTAL_RESETS + TOTAL_FAILURES))
  # This is approximate — objects delivered = total - failed
  DELIVERY_SUCCESS=$(awk "BEGIN {
    if ($TOTAL_OBJECTS > 0) {
      pct = (($TOTAL_OBJECTS - $TOTAL_ISSUES) / $TOTAL_OBJECTS) * 100
      if (pct < 0) pct = 0
      printf \"%.2f\", pct
    } else {
      printf \"0\"
    }
  }")
else
  DELIVERY_SUCCESS="0"
fi

# RSS in MB
RELAY_RSS_MB=$(awk "BEGIN {printf \"%.1f\", $RELAY_RSS_KB / 1024}")

# ── Parse metrics log for additional stats ────────────────────────────────────
UDP_ERRORS_PER_SEC="0"
PKTS_PER_LOOP="0"
QUIC_BYTES_WRITTEN_PER_SEC="0"

if [[ -n "$METRICS_LOG" && -f "$METRICS_LOG" && -s "$METRICS_LOG" ]]; then
  # Extract UDP errors/s average
  UDP_COL=$(head -1 "$METRICS_LOG" | tr '\t' '\n' | grep -n 'UDPErr' | head -1 | cut -d: -f1 || echo "")
  if [[ -n "$UDP_COL" ]]; then
    UDP_ERRORS_PER_SEC=$(tail -n +2 "$METRICS_LOG" | awk -F'\t' -v c="$UDP_COL" '{sum+=$c; n++} END {if(n>0) printf "%.1f", sum/n; else print "0"}')
  fi

  # Extract QUIC bytes written/s average
  QBYTES_COL=$(head -1 "$METRICS_LOG" | tr '\t' '\n' | grep -n 'quicBytesWritten' | head -1 | cut -d: -f1 || echo "")
  if [[ -n "$QBYTES_COL" ]]; then
    QUIC_BYTES_WRITTEN_PER_SEC=$(tail -n +2 "$METRICS_LOG" | awk -F'\t' -v c="$QBYTES_COL" '{sum+=$c; n++} END {if(n>0) printf "%.0f", sum/n; else print "0"}')
  fi
fi

# ── Generate JSON ─────────────────────────────────────────────────────────────
cat > "$OUTPUT" <<EOF
{
  "commit": "$COMMIT",
  "commit_short": "${COMMIT:0:7}",
  "branch": "$BRANCH",
  "timestamp": "$TIMESTAMP",
  "params": {
    "subscriber_max": $SUBSCRIBER_MAX,
    "ramp": $RAMP,
    "duration": $DURATION,
    "io_threads": $IO_THREADS,
    "client_threads": $CLIENT_THREADS,
    "delivery_timeout_ms": $DELIVERY_TIMEOUT,
    "transport": "$TRANSPORT"
  },
  "results": {
    "peak_subscribers": $PEAK_SUBS,
    "total_objects": $TOTAL_OBJECTS,
    "total_bytes": $TOTAL_BYTES,
    "total_resets": $TOTAL_RESETS,
    "total_failures": $TOTAL_FAILURES,
    "delivery_success_pct": $DELIVERY_SUCCESS,
    "throughput_mbps": $THROUGHPUT_MBPS,
    "avg_latency_ms": $AVG_LATENCY_MS,
    "peak_throughput_mbps": $PEAK_MBPS,
    "throughput_per_core_mbps": $THROUGHPUT_PER_CORE,
    "subscribers_per_core": $SUBS_PER_CORE,
    "relay_cpu_pct": $RELAY_CPU,
    "relay_rss_mb": $RELAY_RSS_MB,
    "rss_per_session_kb": $RSS_PER_SESSION,
    "net_throughput_mbps": $NET_THROUGHPUT_MBPS,
    "udp_errors_per_sec": $UDP_ERRORS_PER_SEC,
    "quic_bytes_written_per_sec": $QUIC_BYTES_WRITTEN_PER_SEC,
    "test_result": "$TEST_RESULT",
    "duration_actual_sec": $CLIENT_DURATION
  }
}
EOF

echo "Results JSON written to $OUTPUT"
