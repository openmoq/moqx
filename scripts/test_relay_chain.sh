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
# Usage: bash scripts/test_relay_chain.sh [path/to/o_rly]

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="${1:-$REPO/build/o_rly}"
MOQBIN="$REPO/.scratch/moxygen-install/bin"
DATESERVER="$MOQBIN/moqdateserver"
TEXTCLIENT="$MOQBIN/moqtextclient"

UPSTREAM_PORT=19668
DOWNSTREAM_PORT=19670
UPSTREAM_RELAY_ID="upstream-test"
DOWNSTREAM_RELAY_ID="downstream-test"
NAMESPACE="moq-date"
TIMEOUT=15   # seconds to wait for data

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
sleep 2

# ── Start date server publishing to upstream ───────────────────────────────────
echo "Starting moqdateserver → upstream relay..."
"$DATESERVER" \
  --relay_url="https://localhost:$UPSTREAM_PORT/moq-relay" \
  --ns="$NAMESPACE" \
  --insecure \
  &>/dev/null &
PIDS+=($!)

# Wait for dateserver to announce its namespace and for the downstream relay
# to receive it via the peering handshake.
sleep 3

# ── Subscribe via downstream relay ─────────────────────────────────────────────
echo "Subscribing via downstream relay (timeout ${TIMEOUT}s)..."
timeout "$TIMEOUT" "$TEXTCLIENT" \
  --connect_url="https://localhost:$DOWNSTREAM_PORT/moq-relay" \
  --track_namespace="$NAMESPACE" \
  --track_name="date" \
  --insecure \
  >"$CLIENT_OUT" 2>&1 || true

# ── Verify output ──────────────────────────────────────────────────────────────
# Success: subscribed and received at least one object (no SubscribeError).
if grep -q "SubscribeError" "$CLIENT_OUT" 2>/dev/null; then
  echo "FAIL: downstream relay returned SubscribeError" >&2
  cat "$CLIENT_OUT" >&2
  exit 1
elif grep -q "Largest=" "$CLIENT_OUT" 2>/dev/null; then
  echo "PASS: data flowed through the relay chain"
  grep "Largest=" "$CLIENT_OUT" | head -1
  exit 0
else
  echo "FAIL: no data received from downstream relay" >&2
  echo "Client output:" >&2
  cat "$CLIENT_OUT" >&2
  exit 1
fi
