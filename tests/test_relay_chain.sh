#!/usr/bin/env bash
# test_relay_chain.sh — end-to-end relay chaining test.
#
# Starts two o-rly instances (upstream + downstream), a moqdateserver
# publishing to the upstream relay, and a moqtextclient subscribing from
# the downstream relay. Passes if at least one date object flows through
# the chain within the timeout.
#
# Requires draft 16+ for relay peering (wildcard subscribeNamespace).
# moqdateserver and moqtextclient must be at:
#   .scratch/moxygen-install/bin/  (relative to repo root)
#
# Usage: bash scripts/test_relay_chain.sh [path/to/moqx]

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="${1:-$REPO/build/moqx}"
MOQBIN="${MOQBIN:-$REPO/.scratch/moxygen-install/bin}"
DATESERVER="$MOQBIN/moqdateserver"
TEXTCLIENT="$MOQBIN/moqtextclient"

UPSTREAM_PORT=19668
DOWNSTREAM_PORT=19670
UPSTREAM_RELAY_ID="upstream-test"
DOWNSTREAM_RELAY_ID="downstream-test"
NAMESPACE="moq-date"
NAMESPACE2="moq-date-2"
TIMEOUT=5   # seconds to wait for data

# ── Prereq checks ──────────────────────────────────────────────────────────────
for f in "$BINARY" "$DATESERVER" "$TEXTCLIENT"; do
  if [[ ! -x "$f" ]]; then
    echo "ERROR: not found or not executable: $f" >&2
    exit 1
  fi
done

# ── Temp files ─────────────────────────────────────────────────────────────────
TMPDIR_SCRIPT="$(mktemp -d)"
UPSTREAM_CFG="$TMPDIR_SCRIPT/upstream.yaml"
DOWNSTREAM_CFG="$TMPDIR_SCRIPT/downstream.yaml"
CLIENT_OUT="$TMPDIR_SCRIPT/client.out"
CLIENT_OUT2="$TMPDIR_SCRIPT/client2.out"

# ── Cleanup ────────────────────────────────────────────────────────────────────
PIDS=()
cleanup() {
  for pid in "${PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  for pid in "${PIDS[@]:-}"; do
    wait "$pid" 2>/dev/null || true
  done
  rm -rf "$TMPDIR_SCRIPT"
}
trap cleanup EXIT

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
EOF

# ── Start relays ───────────────────────────────────────────────────────────────
echo "Starting upstream relay on port $UPSTREAM_PORT..."
"$BINARY" --config="$UPSTREAM_CFG" &>/dev/null &
PIDS+=($!)

echo "Starting downstream relay on port $DOWNSTREAM_PORT..."
"$BINARY" --config="$DOWNSTREAM_CFG" &>/dev/null &
PIDS+=($!)

# Give relays time to start and complete the peering handshake.
sleep 1

check_received() {
  local label="$1" out="$2"
  if grep -q "SubscribeError" "$out" 2>/dev/null; then
    echo "FAIL [$label]: SubscribeError" >&2
    cat "$out" >&2
    return 1
  elif grep -q "Largest=" "$out" 2>/dev/null; then
    echo "PASS [$label]: $(grep 'Largest=' "$out" | head -1)"
    return 0
  else
    echo "FAIL [$label]: no data received" >&2
    cat "$out" >&2
    return 1
  fi
}

# ── Direction 1: publisher → upstream, subscriber via downstream ───────────────
echo "Direction 1: moqdateserver → upstream, subscribe via downstream"

"$DATESERVER" \
  --relay_url="https://localhost:$UPSTREAM_PORT/moq-relay" \
  --ns="$NAMESPACE" --insecure \
  &>/dev/null &
PIDS+=($!)

sleep 1

timeout "$TIMEOUT" "$TEXTCLIENT" \
  --connect_url="https://localhost:$DOWNSTREAM_PORT/moq-relay" \
  --track_namespace="$NAMESPACE" --track_name="date" --insecure \
  >"$CLIENT_OUT" 2>&1 || true

check_received "upstream→downstream" "$CLIENT_OUT" || exit 1

# ── Direction 2: publisher → downstream, subscriber via upstream ───────────────
echo "Direction 2: moqdateserver → downstream, subscribe via upstream"

"$DATESERVER" \
  --relay_url="https://localhost:$DOWNSTREAM_PORT/moq-relay" \
  --ns="$NAMESPACE2" --insecure \
  &>/dev/null &
PIDS+=($!)

sleep 1

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
elif grep -q "Largest=" "$CLIENT_OUT" 2>/dev/null; then
  echo "PASS [joining fetch via downstream]: $(grep 'Largest=' "$CLIENT_OUT" | head -1)"
else
  echo "FAIL [joining fetch via downstream]: no data" >&2
  cat "$CLIENT_OUT" >&2
  exit 1
fi

echo "All relay chain tests passed."
