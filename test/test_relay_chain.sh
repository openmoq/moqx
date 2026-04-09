#!/usr/bin/env bash
# test_relay_chain.sh — end-to-end relay chaining test.
#
# Starts two moqx instances (upstream + downstream), a moqdateserver
# publishing to the upstream relay, and a moqtextclient subscribing from
# the downstream relay. Passes if at least one date object flows through
# the chain within the timeout.
#
# Requires draft 16+ for relay peering (wildcard subscribeNamespace).
# moqdateserver and moqtextclient must be at:
#   .scratch/moxygen-install/bin/  (relative to repo root)
#
# Usage: bash scripts/test_relay_chain.sh [path/to/moqx] [--save-logs [dir]]
#   --save-logs [dir]  Save relay DBG4 logs; dir defaults to /tmp/relay_chain_logs

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="${1:-$REPO/build/moqx}"
MOQBIN="${MOQBIN:-$REPO/.scratch/moxygen-install/bin}"

# Parse --save-logs option (may appear anywhere in args)
SAVE_LOGS=false
LOG_DIR="/tmp/relay_chain_logs"
for i in "$@"; do
  if [[ "$i" == "--save-logs" ]]; then
    SAVE_LOGS=true
  elif [[ "$SAVE_LOGS" == "true" && "$i" != --* && "$i" != "$BINARY" ]]; then
    LOG_DIR="$i"
  fi
done
DATESERVER="$MOQBIN/moqdateserver"
TEXTCLIENT="$MOQBIN/moqtextclient"

UPSTREAM_PORT=19668
UPSTREAM_ADMIN_PORT=19669
DOWNSTREAM_PORT=19670
DOWNSTREAM_ADMIN_PORT=19671
UPSTREAM_RELAY_ID="upstream-test"
DOWNSTREAM_RELAY_ID="downstream-test"
NAMESPACE="moq-date"
NAMESPACE2="moq-date-2"
NAMESPACE3="moq-publish"  # for --publish push-mode test
TIMEOUT=2   # seconds to wait for data

# ── Prereq checks ──────────────────────────────────────────────────────────────
for f in "$BINARY" "$DATESERVER" "$TEXTCLIENT"; do
  if [[ ! -x "$f" ]]; then
    echo "ERROR: not found or not executable: $f" >&2
    exit 1
  fi
done

for port in "$UPSTREAM_PORT" "$UPSTREAM_ADMIN_PORT" "$DOWNSTREAM_PORT" "$DOWNSTREAM_ADMIN_PORT"; do
  if ss -ulnp 2>/dev/null | grep -q ":$port "; then
    echo "ERROR: port $port already in use (stale relay process?)" >&2
    ss -ulnp 2>/dev/null | grep ":$port " >&2
    exit 1
  fi
done

# ── Temp files ─────────────────────────────────────────────────────────────────
TMPDIR_SCRIPT="$(mktemp -d)"
UPSTREAM_CFG="$TMPDIR_SCRIPT/upstream.yaml"
DOWNSTREAM_CFG="$TMPDIR_SCRIPT/downstream.yaml"
CLIENT_OUT="$TMPDIR_SCRIPT/client.out"
CLIENT_OUT2="$TMPDIR_SCRIPT/client2.out"
CLIENT_OUT3="$TMPDIR_SCRIPT/client3.out"
DATESERVER_LOG="$TMPDIR_SCRIPT/dateserver.log"
DATESERVER_LOG2="$TMPDIR_SCRIPT/dateserver2.log"

# ── Cleanup ────────────────────────────────────────────────────────────────────
PIDS=()        # non-relay processes (dateserver, etc.) — 2s grace then SIGKILL
RELAY_PIDS=()  # relay processes — wait indefinitely (relay binary has hard 10s watchdog)
cleanup() {
  # Signal everyone.
  for pid in "${PIDS[@]:-}" "${RELAY_PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  # Give non-relay helpers 2s to exit cleanly, then hard-kill survivors.
  local deadline=$(( $(date +%s) + 2 ))
  while true; do
    local any_alive=false
    for pid in "${PIDS[@]:-}"; do
      kill -0 "$pid" 2>/dev/null && { any_alive=true; break; }
    done
    [[ "$any_alive" == true ]] || break
    if (( $(date +%s) >= deadline )); then
      echo "WARNING: helpers did not exit after 2s, sending SIGKILL" >&2
      break
    fi
    sleep 0.2
  done
  for pid in "${PIDS[@]:-}"; do
    if kill -0 "$pid" 2>/dev/null; then
      echo "WARNING: SIGKILL $pid ($(cat /proc/$pid/cmdline 2>/dev/null | tr '\0' ' ' || echo '?'))" >&2
      kill -KILL "$pid" 2>/dev/null || true
    fi
  done
  # Relays: wait indefinitely — relay's hard shutdown watchdog handles any hang.
  wait "${PIDS[@]:-}" "${RELAY_PIDS[@]:-}" 2>/dev/null || true
  rm -rf "$TMPDIR_SCRIPT"
}
trap cleanup EXIT

# ── Readiness helpers ──────────────────────────────────────────────────────────
# wait_ready <admin_port> <label>: wait for admin /info to return 200.
wait_ready() {
  local port="$1" label="$2"
  local deadline=$(( $(date +%s) + 10 ))
  until curl -sf "http://localhost:$port/info" >/dev/null 2>&1; do
    (( $(date +%s) >= deadline )) && { echo "ERROR: $label not ready after 10s" >&2; exit 1; }
    sleep 0.1
  done
}

# check_state <admin_port> <expected_relay_id> <label>: fetch /state, validate JSON
# structure, and echo a summary line.  Exits 1 on failure.
check_state() {
  local port="$1" expected_id="$2" label="$3"
  local url="http://localhost:$port/state"
  local http_code response raw
  raw=$(curl -sf --write-out '\n%{http_code}' "$url" 2>/dev/null) || {
    echo "FAIL [$label /state]: curl failed (exit $?)" >&2; return 1;
  }
  http_code=$(printf '%s' "$raw" | tail -1)
  response=$(printf '%s' "$raw" | head -n -1)
  if [[ "$http_code" != "200" ]]; then
    echo "FAIL [$label /state]: HTTP $http_code" >&2; return 1;
  fi
  if ! printf '%s' "$response" | jq . >/dev/null 2>&1; then
    echo "FAIL [$label /state]: not valid JSON" >&2
    echo "  raw byte count: ${#raw}" >&2
    echo "  http_code: $(printf '%s' "$http_code" | cat -v)" >&2
    echo "  response byte count: ${#response}" >&2
    echo "  response (cat -v): $(printf '%s' "$response" | cat -v)" >&2
    echo "  jq error: $(printf '%s' "$response" | jq . 2>&1 || true)" >&2
    return 1;
  fi
  local relay_id
  relay_id=$(printf '%s' "$response" | jq -r '.relay_id')
  if [[ "$relay_id" != "$expected_id" ]]; then
    echo "FAIL [$label /state]: relay_id=$relay_id, want $expected_id" >&2; return 1;
  fi
  for field in downstream_peers subscriptions; do
    local t
    t=$(printf '%s' "$response" | jq -r ".services.default.${field} | type")
    if [[ "$t" != "array" ]]; then
      echo "FAIL [$label /state]: services.default.$field is $t, want array" >&2; return 1;
    fi
  done
  local tree_type
  tree_type=$(printf '%s' "$response" | jq -r '.services.default.namespace_tree | type')
  if [[ "$tree_type" != "object" ]]; then
    echo "FAIL [$label /state]: namespace_tree is $tree_type, want object" >&2; return 1;
  fi
  local sessions
  sessions=$(printf '%s' "$response" | jq '.active_sessions')
  echo "PASS [$label /state]: relay_id=$relay_id active_sessions=$sessions"
}

# wait_sessions <admin_port> <min> <label>: wait for moqActiveSessions >= min.
wait_sessions() {
  local port="$1" min="$2" label="$3"
  local deadline=$(( $(date +%s) + 10 ))
  local val
  until val=$(curl -sf "http://localhost:$port/metrics" 2>/dev/null \
        | grep "^moqx_moqActiveSessions " | awk '{print $2}') \
        && [[ -n "$val" && "$val" -ge "$min" ]]; do
    (( $(date +%s) >= deadline )) && {
      echo "ERROR: $label: moqActiveSessions=${val:-?} < $min after 10s" >&2; exit 1;
    }
    sleep 0.1
  done
}

# ── Configs ────────────────────────────────────────────────────────────────────
cat >"$UPSTREAM_CFG" <<EOF
relay_id: "$UPSTREAM_RELAY_ID"
listeners:
  - name: upstream
    udp:
      socket:
        address: "::"
        port: $UPSTREAM_PORT
    tls:
      insecure: true
    endpoint: "/moq-relay"
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache:
      enabled: false
      max_tracks: 100
      max_groups_per_track: 3
admin:
  port: $UPSTREAM_ADMIN_PORT
  address: "::"
  plaintext: true
EOF

cat >"$DOWNSTREAM_CFG" <<EOF
relay_id: "$DOWNSTREAM_RELAY_ID"
listeners:
  - name: downstream
    udp:
      socket:
        address: "::"
        port: $DOWNSTREAM_PORT
    tls:
      insecure: true
    endpoint: "/moq-relay"
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache:
      enabled: false
      max_tracks: 100
      max_groups_per_track: 3
    upstream:
      url: "moqt://localhost:$UPSTREAM_PORT/moq-relay"
      tls:
        insecure: true
      idle_timeout_ms: 60000
admin:
  port: $DOWNSTREAM_ADMIN_PORT
  address: "::"
  plaintext: true
EOF

# ── Start relays ───────────────────────────────────────────────────────────────
if [[ "$SAVE_LOGS" == "true" ]]; then
  mkdir -p "$LOG_DIR"
  UPSTREAM_LOG="$LOG_DIR/upstream.log"
  DOWNSTREAM_LOG="$LOG_DIR/downstream.log"
  DATESERVER_LOG="$LOG_DIR/dateserver.log"
  DATESERVER_LOG2="$LOG_DIR/dateserver2.log"
  echo "Saving relay logs to $LOG_DIR"
  RELAY_LOG_ARGS="--logging=DBG4"
  DATESERVER_LOG_ARGS="--logging=DBG4"
else
  UPSTREAM_LOG="/dev/null"
  DOWNSTREAM_LOG="/dev/null"
  RELAY_LOG_ARGS=""
  DATESERVER_LOG_ARGS=""
fi

echo "Starting upstream relay on port $UPSTREAM_PORT..."
"$BINARY" --config="$UPSTREAM_CFG" $RELAY_LOG_ARGS >"$UPSTREAM_LOG" 2>&1 &
RELAY_PIDS+=($!)

echo "Starting downstream relay on port $DOWNSTREAM_PORT..."
"$BINARY" --config="$DOWNSTREAM_CFG" $RELAY_LOG_ARGS >"$DOWNSTREAM_LOG" 2>&1 &
RELAY_PIDS+=($!)

# Wait for relays to start and complete the peering handshake.
wait_ready "$UPSTREAM_ADMIN_PORT" "upstream"
wait_ready "$DOWNSTREAM_ADMIN_PORT" "downstream"
wait_sessions "$UPSTREAM_ADMIN_PORT" 1 "peering"

check_received() {
  local label="$1" out="$2"
  if grep -q "SubscribeError" "$out" 2>/dev/null; then
    echo "FAIL [$label]: SubscribeError" >&2
    cat "$out" >&2
    echo "--- dateserver log ---" >&2
    cat "$DATESERVER_LOG" 2>/dev/null >&2 || true
    return 1
  elif grep -qE "^[0-9]" "$out" 2>/dev/null; then
    echo "PASS [$label]: $(grep -E "^[0-9]" "$out" | head -1)"
    return 0
  else
    echo "FAIL [$label]: no data received" >&2
    cat "$out" >&2
    echo "--- dateserver log ---" >&2
    cat "$DATESERVER_LOG" 2>/dev/null >&2 || true
    return 1
  fi
}

# ── Direction 1: publisher → upstream, subscriber via downstream ───────────────
echo "Direction 1: moqdateserver → upstream, subscribe via downstream"

"$DATESERVER" \
  --relay_url="https://localhost:$UPSTREAM_PORT/moq-relay" \
  --ns="$NAMESPACE" --insecure $DATESERVER_LOG_ARGS \
  >"$DATESERVER_LOG" 2>&1 &
PIDS+=($!)

wait_sessions "$UPSTREAM_ADMIN_PORT" 2 "dir1 dateserver"

timeout "$TIMEOUT" "$TEXTCLIENT" \
  --connect_url="https://localhost:$DOWNSTREAM_PORT/moq-relay" \
  --track_namespace="$NAMESPACE" --track_name="date" --insecure \
  >"$CLIENT_OUT" 2>&1 || true

check_received "upstream→downstream" "$CLIENT_OUT" || exit 1

# ── /state snapshot: publisher connected, peering active ──────────────────────
# dateserver is still publishing to upstream; downstream is still peered.
# Upstream should show: the publish subscription, "moq-date" in namespace_tree,
# and the downstream relay in downstream_peers.
# Downstream should show: upstream.state=connected.
echo "Checking /state on both relays while publisher is active..."
check_state "$UPSTREAM_ADMIN_PORT" "$UPSTREAM_RELAY_ID" "upstream"
check_state "$DOWNSTREAM_ADMIN_PORT" "$DOWNSTREAM_RELAY_ID" "downstream"

# Upstream-specific: downstream relay must appear as a peer.
UP_STATE=$(curl -sf "http://localhost:$UPSTREAM_ADMIN_PORT/state")
PEER_COUNT=$(printf '%s' "$UP_STATE" | jq '.services.default.downstream_peers | length')
if [[ "$PEER_COUNT" -lt 1 ]]; then
  echo "FAIL [upstream /state]: expected >=1 downstream_peers, got $PEER_COUNT" >&2; exit 1;
fi
echo "PASS [upstream /state]: downstream_peers=$PEER_COUNT"

# Upstream-specific: "moq-date" namespace must appear in the tree.
NS_PRESENT=$(printf '%s' "$UP_STATE" | jq '.services.default.namespace_tree.children | has("moq-date")')
if [[ "$NS_PRESENT" != "true" ]]; then
  echo "FAIL [upstream /state]: namespace_tree missing moq-date child" >&2; exit 1;
fi
echo "PASS [upstream /state]: namespace_tree contains moq-date"

# Downstream-specific: upstream must be reported connected.
DOWN_STATE=$(curl -sf "http://localhost:$DOWNSTREAM_ADMIN_PORT/state")
UP_CONN=$(printf '%s' "$DOWN_STATE" | jq -r '.services.default.upstream.state')
if [[ "$UP_CONN" != "connected" ]]; then
  echo "FAIL [downstream /state]: upstream.state=$UP_CONN, want connected" >&2; exit 1;
fi
echo "PASS [downstream /state]: upstream.state=connected"

# ── Direction 2: publisher → downstream, subscriber via upstream ───────────────
echo "Direction 2: moqdateserver → downstream, subscribe via upstream"

"$DATESERVER" \
  --relay_url="https://localhost:$DOWNSTREAM_PORT/moq-relay" \
  --ns="$NAMESPACE2" --insecure $DATESERVER_LOG_ARGS \
  >"$DATESERVER_LOG2" 2>&1 &
PIDS+=($!)

wait_sessions "$DOWNSTREAM_ADMIN_PORT" 1 "dir2 dateserver"

timeout "$TIMEOUT" "$TEXTCLIENT" \
  --connect_url="https://localhost:$UPSTREAM_PORT/moq-relay" \
  --track_namespace="$NAMESPACE2" --track_name="date" --insecure \
  >"$CLIENT_OUT2" 2>&1 || true

check_received "downstream→upstream" "$CLIENT_OUT2" || exit 1

# ── Direction 3: joining fetch via relay chain ─────────────────────────────────
# moq-date dateserver from direction 1 is still running on upstream.
# --jrfetch --join_start=1: fetches 1 group back while also subscribing forward.
# Exercises both fetch and subscribe forwarding through the relay chain.
echo "Direction 3: joining fetch via downstream relay"

timeout "$TIMEOUT" "$TEXTCLIENT" \
  --connect_url="https://localhost:$DOWNSTREAM_PORT/moq-relay" \
  --track_namespace="$NAMESPACE" --track_name="date" --insecure \
  --jrfetch --join_start=1 \
  >"$CLIENT_OUT" 2>&1 || true

if grep -qE "SubscribeError|Fetch.*failed" "$CLIENT_OUT" 2>/dev/null; then
  echo "FAIL [joining fetch via downstream]: error" >&2
  cat "$CLIENT_OUT" >&2
  exit 1
elif grep -qE "^[0-9]" "$CLIENT_OUT" 2>/dev/null; then
  echo "PASS [joining fetch via downstream]: $(grep -E "^[0-9]" "$CLIENT_OUT" | head -1)"
else
  echo "FAIL [joining fetch via downstream]: no data" >&2
  cat "$CLIENT_OUT" >&2
  exit 1
fi

# ── Direction 4: --publish push mode through relay chain ───────────────────────
# textclient registers subscribeNamespace on downstream; dateserver --publish
# pushes via PUBLISH to upstream; relay chain routes it to textclient.
echo "Direction 4: --publish push mode (upstream→downstream)"

# Start textclient --publish first so it registers subscribeNamespace.
timeout "$TIMEOUT" "$TEXTCLIENT" \
  --connect_url="https://localhost:$DOWNSTREAM_PORT/moq-relay" \
  --track_namespace="$NAMESPACE3" --track_name="date" --insecure \
  --publish \
  >"$CLIENT_OUT3" 2>&1 &
TCPID=$!

wait_sessions "$DOWNSTREAM_ADMIN_PORT" 2 "dir4 textclient"

# dateserver --publish: connects to upstream and pushes via PUBLISH.
"$DATESERVER" \
  --relay_url="https://localhost:$UPSTREAM_PORT/moq-relay" \
  --ns="$NAMESPACE3" --insecure --publish \
  &>/dev/null &
PIDS+=($!)

wait $TCPID || true

# Success: textclient received date objects printed to stdout by onObject().
if grep -qE "^[0-9]" "$CLIENT_OUT3" 2>/dev/null; then
  echo "PASS [--publish mode]: $(grep -E "^[0-9]" "$CLIENT_OUT3" | head -1)"
else
  echo "FAIL [--publish mode]: no data" >&2
  cat "$CLIENT_OUT3" >&2
  exit 1
fi

echo "All relay chain tests passed."
