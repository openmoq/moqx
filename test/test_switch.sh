#!/usr/bin/env bash
set -euo pipefail

MOQX_BIN="${1:-$(dirname "$0")/../build/moqx}"
MOQBIN="${MOQBIN:-$(dirname "$0")/../.scratch/moxygen-install/bin}"
TIMEOUT="${TIMEOUT:-30}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

source "$SCRIPT_DIR/test_ports.sh"

RELAY_PORT=$TEST_SWITCH_RELAY
ADMIN_PORT=$TEST_SWITCH_ADMIN
TMPDIR=$(mktemp -d)
trap 'kill "${RELAY_PID:-}" "${PUB_PID:-}" 2>/dev/null || true; rm -rf "$TMPDIR"' EXIT

# ---- generate relay config ----
RELAY_CFG="$TMPDIR/relay.yaml"
cat > "$RELAY_CFG" <<YAML
relay_id: "switch-test"
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
      max_groups_per_track: 25
admin:
  port: $ADMIN_PORT
  address: "::"
  plaintext: true
YAML

# ---- start relay ----
"$MOQX_BIN" --config="$RELAY_CFG" >"$TMPDIR/relay.log" 2>&1 &
RELAY_PID=$!

# Wait for relay admin endpoint (mirrors test_relay_chain.sh wait_ready pattern).
for i in $(seq 1 100); do
  curl -sf "http://localhost:$ADMIN_PORT/info" >/dev/null 2>&1 && break
  sleep 0.1
done
curl -sf "http://localhost:$ADMIN_PORT/info" >/dev/null 2>&1 || {
  echo "ERROR: relay did not start. Log:"; cat "$TMPDIR/relay.log"; exit 1
}

# ---- start publisher ----
"$MOQBIN/moqswitchpub" \
  --relay_url="https://localhost:$RELAY_PORT/moq-relay" \
  --ns="test" --interval_ms=100 --insecure \
  >"$TMPDIR/pub.log" 2>&1 &
PUB_PID=$!

# Wait until publisher session appears in relay metrics (mirrors wait_sessions in relay chain test).
for i in $(seq 1 100); do
  val=$(curl -sf "http://localhost:$ADMIN_PORT/metrics" 2>/dev/null \
        | grep "^moqx_moqActiveSessions " | awk '{print $2}')
  [[ -n "$val" && "$val" -ge 1 ]] && break
  sleep 0.1
done
val=$(curl -sf "http://localhost:$ADMIN_PORT/metrics" 2>/dev/null \
      | grep "^moqx_moqActiveSessions " | awk '{print $2}')
if [[ -z "$val" || "$val" -lt 1 ]]; then
  echo "ERROR: publisher did not connect (moqActiveSessions=${val:-?})"
  echo "--- relay log ---"; cat "$TMPDIR/relay.log"
  echo "--- pub log ---"; cat "$TMPDIR/pub.log"
  exit 1
fi
sleep 0.5  # allow publisher to complete PUBLISH + start ticking

# ---- scenario 1: clean switch ----
echo "Running scenario 1: clean switch..."
SUB1_OUT="$TMPDIR/sub1.json"
timeout "$TIMEOUT" "$MOQBIN/moqswitchsub" \
  --relay_url="https://localhost:$RELAY_PORT/moq-relay" \
  --ns="test" --warm_up_groups=5 --lag_seconds=0 --collect_groups=10 \
  --insecure >"$SUB1_OUT" 2>"$TMPDIR/sub1.err" || true

RESULT1=$(grep '"event":"result"' "$SUB1_OUT" | tail -1 || true)
if [ -z "$RESULT1" ]; then
  echo "ERROR: scenario 1 produced no result line."
  echo "--- stdout ---"; cat "$SUB1_OUT"
  echo "--- stderr ---"; cat "$TMPDIR/sub1.err"
  echo "--- relay log ---"; tail -50 "$TMPDIR/relay.log"
  exit 1
fi
PASS1=$(echo "$RESULT1" | python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(str(d.get('pass',False)).lower())")
G_SWITCH1=$(echo "$RESULT1" | python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(d.get('g_switch',0))")
if [ "$PASS1" != "true" ]; then
  echo "FAIL scenario 1: $RESULT1"; cat "$SUB1_OUT"; exit 1
fi
echo "SWITCH test passed (clean): g_switch=$G_SWITCH1 $(echo "$RESULT1" | python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print('gap='+str(d['gap']).lower()+' duplicate='+str(d['duplicate']).lower())")"

# ---- scenario 2: switch under lag ----
echo "Running scenario 2: switch under lag (2s)..."
SUB2_OUT="$TMPDIR/sub2.json"
timeout "$TIMEOUT" "$MOQBIN/moqswitchsub" \
  --relay_url="https://localhost:$RELAY_PORT/moq-relay" \
  --ns="test" --warm_up_groups=5 --lag_seconds=2 --collect_groups=10 \
  --insecure >"$SUB2_OUT" 2>"$TMPDIR/sub2.err" || true

RESULT2=$(grep '"event":"result"' "$SUB2_OUT" | tail -1 || true)
if [ -z "$RESULT2" ]; then
  echo "ERROR: scenario 2 produced no result line."
  echo "--- stdout ---"; cat "$SUB2_OUT"
  echo "--- stderr ---"; cat "$TMPDIR/sub2.err"
  echo "--- relay log ---"; tail -50 "$TMPDIR/relay.log"
  exit 1
fi
PASS2=$(echo "$RESULT2" | python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(str(d.get('pass',False)).lower())")
CATCHUP=$(echo "$RESULT2" | python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(d.get('catchup_groups',0))")
G_SWITCH2=$(echo "$RESULT2" | python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(d.get('g_switch',0))")
LIVE_EDGE=$(echo "$RESULT2" | python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(d.get('live_edge',0))")
if [ "$PASS2" != "true" ]; then
  echo "FAIL scenario 2: $RESULT2"; cat "$SUB2_OUT"; exit 1
fi
# At 10 groups/sec, 2s lag → ~20 groups behind. Require at least 15.
if [ "$CATCHUP" -lt 15 ]; then
  echo "FAIL scenario 2: expected catchup_groups>=15 but got $CATCHUP"; cat "$SUB2_OUT"; exit 1
fi
echo "SWITCH test passed (lag):   g_switch=$G_SWITCH2 live_edge=$LIVE_EDGE catchup_groups=$CATCHUP gap=false duplicate=false"

exit 0
