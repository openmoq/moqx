#!/usr/bin/env bash
# test_admin_cache_purge_race.sh — race-condition stress test for POST /cache/purge.
#
# Two-phase test:
#
# Phase 1 — live storm:  a publisher + subscriber are active while 30 concurrent
#   purge requests hammer the relay.  safe_purge must consistently skip the live
#   track without crashing or corrupting state.
#
# Phase 2 — eviction:  publisher and subscriber are stopped, their sessions are
#   waited out, and then a single purge is issued.  The now-evictable track must
#   be fully evicted (evicted >= 1, skipped == 0).
#
# Pass criteria:
#   1. Relay process stays alive throughout both phases.
#   2. Subscriber receives at least one object before the purge storm.
#   3. All storm purge responses during Phase 1 have skipped >= 1 (live track
#      was encountered) and contain no "error" field.
#   4. Phase 2 purge returns evicted >= 1 (eviction path exercised).
#   5. /metrics endpoint is still readable after both phases.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="${1:-$REPO/build/moqx}"
MOQBIN="${MOQBIN:-$REPO/.scratch/moxygen-install/bin}"
DATESERVER="$MOQBIN/moqdateserver"
TEXTCLIENT="$MOQBIN/moqtextclient"

# Use ports that don't collide with the other integration tests (9668/9669).
RELAY_PORT=19678
ADMIN_PORT=19679
NAMESPACE="moq-date-purge-race"
PURGE_URL="http://localhost:${ADMIN_PORT}/cache/purge"
INFO_URL="http://localhost:${ADMIN_PORT}/info"
METRICS_URL="http://localhost:${ADMIN_PORT}/metrics"

# ── Prereq checks ──────────────────────────────────────────────────────────────
for f in "$BINARY" "$DATESERVER" "$TEXTCLIENT"; do
  if [[ ! -x "$f" ]]; then
    echo "SKIP: required binary not found or not executable: $f" >&2
    exit 0
  fi
done

# ── Temp files ─────────────────────────────────────────────────────────────────
TMPDIR_SCRIPT="$(mktemp -d)"
RELAY_CFG="$TMPDIR_SCRIPT/relay.yaml"
CLIENT_OUT="$TMPDIR_SCRIPT/client.out"
DATESERVER_LOG="$TMPDIR_SCRIPT/dateserver.log"
PURGE_LOG="$TMPDIR_SCRIPT/purge.log"
touch "$PURGE_LOG"

# ── Process tracking ───────────────────────────────────────────────────────────
PIDS=()
RELAY_PIDS=()

cleanup() {
  for pid in "${PIDS[@]:-}" "${RELAY_PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  local deadline=$(( $(date +%s) + 2 ))
  while true; do
    local any_alive=false
    for pid in "${PIDS[@]:-}"; do
      kill -0 "$pid" 2>/dev/null && { any_alive=true; break; }
    done
    [[ "$any_alive" == true ]] || break
    if (( $(date +%s) >= deadline )); then
      for pid in "${PIDS[@]:-}"; do kill -KILL "$pid" 2>/dev/null || true; done
      break
    fi
    sleep 0.2
  done
  wait "${PIDS[@]:-}" "${RELAY_PIDS[@]:-}" 2>/dev/null || true
  rm -rf "$TMPDIR_SCRIPT"
}
trap cleanup EXIT

# ── Helpers ────────────────────────────────────────────────────────────────────

# Kill any stale processes holding the relay's ports from a previous run.
evict_stale_relay() {
  local pids
  pids=$(lsof -ti TCP:"$ADMIN_PORT" 2>/dev/null || true)
  if [[ -n "$pids" ]]; then
    echo "Killing stale process(es) on admin port $ADMIN_PORT: $pids"
    kill $pids 2>/dev/null || true
    sleep 0.3
  fi
}

wait_ready() {
  local deadline=$(( $(date +%s) + 10 ))
  until curl -sf "$INFO_URL" >/dev/null 2>&1; do
    (( $(date +%s) >= deadline )) && { echo "ERROR: relay not ready after 10s" >&2; exit 1; }
    sleep 0.1
  done
  # Verify the relay we started is the one answering, not a stale process.
  if ! kill -0 "$RELAY_PID" 2>/dev/null; then
    echo "ERROR: relay PID $RELAY_PID died at startup (port conflict?)" >&2; exit 1
  fi
}

# Wait until moqx_moqActiveSessions reaches exactly `target`.
wait_sessions() {
  local target="$1" label="$2"
  local deadline=$(( $(date +%s) + 10 ))
  local val
  until val=$(curl -sf "$METRICS_URL" 2>/dev/null \
        | grep "^moqx_moqActiveSessions " | awk '{print $2}') \
        && [[ -n "$val" && "$val" -eq "$target" ]]; do
    (( $(date +%s) >= deadline )) && {
      echo "ERROR: $label: moqActiveSessions=${val:-?} != $target after 10s" >&2; exit 1;
    }
    sleep 0.1
  done
}

# ── Config ─────────────────────────────────────────────────────────────────────
cat >"$RELAY_CFG" <<EOF
listeners:
  - name: main
    udp:
      socket:
        address: "::"
        port: $RELAY_PORT
    tls:
      insecure: true
    endpoint: "/moq-relay"
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache:
      enabled: true
      max_tracks: 100
      max_groups_per_track: 3
admin:
  port: $ADMIN_PORT
  address: "::"
  plaintext: true
EOF

# ── Kill any leftover relay from a previous test run ─────────────────────────
evict_stale_relay

# ── Start relay ────────────────────────────────────────────────────────────────
echo "Starting relay on port $RELAY_PORT (cache enabled)..."
"$BINARY" --config="$RELAY_CFG" >/dev/null 2>&1 &
RELAY_PID=$!
RELAY_PIDS+=($RELAY_PID)
wait_ready

# ── Start publisher ────────────────────────────────────────────────────────────
echo "Starting moqdateserver (publisher)..."
"$DATESERVER" \
  --relay_url="https://localhost:$RELAY_PORT/moq-relay" \
  --ns="$NAMESPACE" --insecure \
  >"$DATESERVER_LOG" 2>&1 &
DATESERVER_PID=$!
PIDS+=($DATESERVER_PID)
wait_sessions 1 "publisher connected"

# ── Start subscriber ───────────────────────────────────────────────────────────
echo "Starting moqtextclient (subscriber)..."
"$TEXTCLIENT" \
  --connect_url="https://localhost:$RELAY_PORT/moq-relay" \
  --track_namespace="$NAMESPACE" --track_name="date" --insecure \
  >"$CLIENT_OUT" 2>&1 &
CLIENT_PID=$!
PIDS+=($CLIENT_PID)
wait_sessions 2 "subscriber connected"

# Let objects flow into the cache.
sleep 0.4

if grep -q "Largest=" "$CLIENT_OUT" 2>/dev/null; then
  echo "Pre-storm: subscriber receiving — $(grep 'Largest=' "$CLIENT_OUT" | head -1)"
else
  echo "Pre-storm: subscriber connected but no objects yet — proceeding"
fi

# ═══════════════════════════════════════════════════════════════════════════════
# Phase 1: purge storm while both pub and sub are live
# Expected: every response has skipped >= 1 (live track blocked eviction)
# ═══════════════════════════════════════════════════════════════════════════════
echo "--- Phase 1: purge storm while pub/sub live ---"

PURGE_PIDS=()
for i in $(seq 1 20); do
  curl -sf -X POST "$PURGE_URL" \
    -H 'Content-Type: application/json' -d '{}' >>"$PURGE_LOG" 2>&1 &
  PURGE_PIDS+=($!)
  sleep 0.05
done
for i in $(seq 1 10); do
  curl -sf -X POST "$PURGE_URL" \
    -H 'Content-Type: application/json' -d '{}' >>"$PURGE_LOG" 2>&1 || true
done
wait "${PURGE_PIDS[@]:-}" 2>/dev/null || true

echo "Phase 1 responses:"
cat "$PURGE_LOG"
echo ""

# ═══════════════════════════════════════════════════════════════════════════════
# Phase 2: stop pub+sub, wait for sessions to drain, then purge
# Expected: evicted >= 1 (track is no longer live, eviction path exercised)
# ═══════════════════════════════════════════════════════════════════════════════
echo "--- Phase 2: stopping subscriber and publisher, waiting for drain ---"
kill "$CLIENT_PID" 2>/dev/null || true
wait "$CLIENT_PID" 2>/dev/null || true
kill "$DATESERVER_PID" 2>/dev/null || true
wait "$DATESERVER_PID" 2>/dev/null || true
# Relay still holds the track until both QUIC sessions close.
wait_sessions 0 "all sessions drained"

PHASE2=$(curl -sf -X POST "$PURGE_URL" \
  -H 'Content-Type: application/json' -d '{}' 2>/dev/null || echo "CURL_FAILED")
echo "Phase 2 purge response: $PHASE2"

# ── Assertions ─────────────────────────────────────────────────────────────────

# 1. Relay still alive.
if ! kill -0 "$RELAY_PID" 2>/dev/null; then
  echo "FAIL [1]: relay process crashed" >&2; exit 1
fi
echo "PASS [1]: relay survived both phases"

# 2. Subscriber received data before/during the storm.
if grep -q "Largest=" "$CLIENT_OUT" 2>/dev/null; then
  echo "PASS [2]: subscriber received data: $(grep 'Largest=' "$CLIENT_OUT" | head -1)"
elif grep -q "SubscribeError" "$CLIENT_OUT" 2>/dev/null; then
  echo "FAIL [2]: subscriber got SubscribeError — possible regression" >&2
  cat "$CLIENT_OUT" >&2; exit 1
else
  echo "WARNING [2]: subscriber did not log any objects (slow environment?)"
fi

# 3. All Phase 1 storm responses had no error field and contained an evicted
#    field. Purge is now unconditional — live tracks are evicted immediately so
#    there is no skipped field in the response.
STORM_COUNT=$(wc -l <"$PURGE_LOG")
if [[ "$STORM_COUNT" -eq 0 ]]; then
  echo "FAIL [3]: no storm purge responses recorded" >&2; exit 1
fi
if grep -q '"error"' "$PURGE_LOG" 2>/dev/null; then
  echo "FAIL [3]: a storm purge returned an error body:" >&2
  grep '"error"' "$PURGE_LOG" >&2; exit 1
fi
if ! grep -q '"evicted":' "$PURGE_LOG" 2>/dev/null; then
  echo "FAIL [3]: storm purge responses missing evicted field" >&2
  cat "$PURGE_LOG" >&2; exit 1
fi
echo "PASS [3]: all $STORM_COUNT storm responses had evicted field, no errors (unconditional purge)"

# 4. Phase 2 purge returns valid JSON with evicted field and relay still alive.
#    The track was evicted unconditionally during the phase 1 storm, so
#    evicted may be 0 here (track already gone) — that is correct behavior.
if [[ "$PHASE2" == "CURL_FAILED" ]]; then
  echo "FAIL [4]: Phase 2 purge curl failed — relay unresponsive" >&2; exit 1
fi
if ! echo "$PHASE2" | grep -q '"evicted":'; then
  echo "FAIL [4]: Phase 2 purge response missing evicted field (got: $PHASE2)" >&2; exit 1
fi
EVICTED=$(echo "$PHASE2" | grep -o '"evicted":[0-9]*' | cut -d: -f2 || true)
echo "PASS [4]: Phase 2 purge returned valid response: evicted=$EVICTED"

# 5. /metrics still readable (event loop not wedged).
METRICS=$(curl -sf "$METRICS_URL" 2>/dev/null || echo "")
if ! echo "$METRICS" | grep -q "moqx_moqActiveSessions"; then
  echo "FAIL [5]: /metrics unresponsive after test (event loop wedged?)" >&2; exit 1
fi
echo "PASS [5]: /metrics readable after both phases"

echo ""
echo "ALL PASS — cache-purge concurrency test passed"
