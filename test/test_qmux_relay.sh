#!/usr/bin/env bash
# test_qmux_relay.sh — end-to-end test of a moqx relay's proxygen_qmux listener.
#
# Starts one moqx relay with a single quic_stack: proxygen_qmux listener
# (QMUX-on-TCP + Fizz, insecure), a moqdateserver publishing to it over qmux,
# and a moqtextclient subscribing from it over qmux. Passes if at least one
# date object flows through the relay within the timeout.
#
# The moqtest_* binaries can't drive this: their client side speaks only
# QUIC/WebTransport. moqdateserver/moqtextclient have a real client-side
# --qmux (makeRelayClientTransport TransportType::QMUX), so they're used here.
#
# moqdateserver/moqtextclient must be at:
#   .scratch/moxygen-install/bin/  (relative to repo root)
#
# Usage: bash test/test_qmux_relay.sh [path/to/moqx]

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
# Explicit arg wins; otherwise pick the most recently built moqx, so a fresh
# build-san/ is preferred over a stale build/ that may predate proxygen_qmux.
BINARY="${1:-}"
if [[ -z "$BINARY" ]]; then
  BINARY="$(ls -t "$REPO"/build*/moqx 2>/dev/null | head -1 || true)"
  BINARY="${BINARY:-$REPO/build/moqx}"
fi
MOQBIN="${MOQBIN:-$REPO/.scratch/moxygen-install/bin}"
# shellcheck source=test_ports.sh
source "$REPO/test/test_ports.sh"
# shellcheck source=test_versions.sh
source "$REPO/test/test_versions.sh"

# Resolve a qmux-capable sample binary. Prefer MOQBIN's flat layout (the install
# at .scratch/moxygen-install/bin); if that's a pre-qmux release, fall back to a
# from-source build under .scratch/standalone-build*/moxygen/samples (e.g. the
# asan build), where samples live in per-tool subdirs. Empty if none found.
# $1 = binary name, $2 = samples subdir for the fallback layout.
resolve_qmux_bin() {
  local name="$1" sub="$2" cand
  for cand in "$MOQBIN/$name" \
              "$REPO"/.scratch/standalone-build*/moxygen/samples/"$sub/$name"; do
    if [[ -x "$cand" ]] && grep -q "qmux" <<<"$("$cand" --help 2>&1 || true)"; then
      echo "$cand"
      return
    fi
  done
}

DATESERVER="$(resolve_qmux_bin moqdateserver date)"
TEXTCLIENT="$(resolve_qmux_bin moqtextclient text-client)"

RELAY_PORT=$TEST_QMUX_RELAY_LISTEN
ADMIN_PORT=$TEST_QMUX_RELAY_ADMIN
DATESERVER_PORT=$TEST_QMUX_DATESERVER_LISTEN
NAMESPACE="moq-date"
TIMEOUT=3   # seconds to wait for data

# ── Prereq checks ───────────────────────────────────────────────────────────────
if [[ ! -x "$BINARY" ]]; then
  echo "ERROR: not found or not executable: $BINARY" >&2
  exit 1
fi

# Skip (ctest SKIP_RETURN_CODE 77) when no qmux-capable moqdateserver/moqtextclient
# is available — the sample binaries only gained client-side --qmux after moxygen's
# qmux work, so a pre-qmux install (and no from-source build) can't drive this.
if [[ -z "$DATESERVER" || -z "$TEXTCLIENT" ]]; then
  echo "SKIP: no qmux-capable moqdateserver/moqtextclient found (pre-qmux build)" >&2
  exit 77
fi
echo "Using publisher: $DATESERVER"
echo "Using subscriber: $TEXTCLIENT"

# qmux is TCP — check TCP listeners (relay chain checks UDP).
for port in "$RELAY_PORT" "$ADMIN_PORT" "$DATESERVER_PORT"; do
  if ss -tlnp 2>/dev/null | grep -q ":$port "; then
    echo "ERROR: port $port already in use (stale process?)" >&2
    exit 1
  fi
done

# ── Temp files / cleanup ────────────────────────────────────────────────────────
TMPDIR_SCRIPT="$(mktemp -d)"
RELAY_CFG="$TMPDIR_SCRIPT/relay.yaml"
CLIENT_OUT="$TMPDIR_SCRIPT/client.out"
DATESERVER_LOG="$TMPDIR_SCRIPT/dateserver.log"

PIDS=()        # helpers — 2s grace then SIGKILL
RELAY_PIDS=()  # relay — wait indefinitely (relay has a hard shutdown watchdog)
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
    (( $(date +%s) >= deadline )) && break
    sleep 0.2
  done
  for pid in "${PIDS[@]:-}"; do
    kill -KILL "$pid" 2>/dev/null || true
  done
  wait "${PIDS[@]:-}" "${RELAY_PIDS[@]:-}" 2>/dev/null || true
  rm -rf "$TMPDIR_SCRIPT"
}
trap cleanup EXIT

# ── Readiness helpers ───────────────────────────────────────────────────────────
wait_ready() {
  local port="$1" label="$2"
  local deadline=$(( $(date +%s) + 10 ))
  until curl -sf "http://localhost:$port/info" >/dev/null 2>&1; do
    (( $(date +%s) >= deadline )) && { echo "ERROR: $label not ready after 10s" >&2; exit 1; }
    sleep 0.1
  done
}

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

# ── Relay config: a single proxygen_qmux listener, insecure ─────────────────────
cat >"$RELAY_CFG" <<EOF
relay_id: "qmux-test"
listeners:
  - name: qmux
    quic_stack: proxygen_qmux
    udp:
      socket:
        address: "::"
        port: $RELAY_PORT
    tls:
      insecure: true
    endpoint: "/moq-relay"
    moqt_versions: ${MOQT_TEST_VERSIONS}
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
  port: $ADMIN_PORT
  address: "::"
  plaintext: true
EOF

echo "Starting moqx relay (proxygen_qmux) on port $RELAY_PORT..."
"$BINARY" --config="$RELAY_CFG" >/dev/null 2>&1 &
RELAY_PIDS+=($!)
wait_ready "$ADMIN_PORT" "relay"

# ── Publisher: moqdateserver connects to the relay over qmux ────────────────────
# --quic=false so only a throwaway qmux listener binds; --qmux selects QMUX as
# the relay-client transport (makeRelayClientTransport).
echo "Starting moqdateserver (qmux publisher)..."
"$DATESERVER" \
  --relay_url="https://localhost:$RELAY_PORT/moq-relay" \
  --ns="$NAMESPACE" --qmux --quic=false --port="$DATESERVER_PORT" --insecure \
  >"$DATESERVER_LOG" 2>&1 &
PIDS+=($!)
wait_sessions "$ADMIN_PORT" 1 "publisher"

# ── Subscriber: moqtextclient subscribes from the relay over qmux ───────────────
# Poll the output and stop as soon as the first object (or an error) appears,
# rather than waiting the full timeout — the date track emits one object/sec.
echo "Starting moqtextclient (qmux subscriber)..."
"$TEXTCLIENT" \
  --connect_url="https://localhost:$RELAY_PORT/moq-relay" \
  --track_namespace="$NAMESPACE" --track_name="date" --qmux --insecure \
  >"$CLIENT_OUT" 2>&1 &
TCPID=$!
PIDS+=($TCPID)
deadline=$(( $(date +%s) + TIMEOUT ))
while (( $(date +%s) < deadline )); do
  grep -qE "^[0-9]|SubscribeError" "$CLIENT_OUT" 2>/dev/null && break
  kill -0 "$TCPID" 2>/dev/null || break
  sleep 0.1
done
kill "$TCPID" 2>/dev/null || true

# ── Verify ──────────────────────────────────────────────────────────────────────
if grep -q "SubscribeError" "$CLIENT_OUT" 2>/dev/null; then
  echo "FAIL [qmux relay]: SubscribeError" >&2
  cat "$CLIENT_OUT" >&2
  echo "--- dateserver log ---" >&2; cat "$DATESERVER_LOG" >&2 || true
  exit 1
elif grep -qE "^[0-9]" "$CLIENT_OUT" 2>/dev/null; then
  echo "PASS [qmux relay]: received '$(grep -E "^[0-9]" "$CLIENT_OUT" | head -1)'"
  exit 0
else
  echo "FAIL [qmux relay]: no data received over qmux" >&2
  cat "$CLIENT_OUT" >&2
  echo "--- dateserver log ---" >&2; cat "$DATESERVER_LOG" >&2 || true
  exit 1
fi
