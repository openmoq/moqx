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
listeners:
  - port: $RELAY_PORT
    cert: $SCRIPT_DIR/test_cert.pem
    key: $SCRIPT_DIR/test_key.pem
admin:
  port: $ADMIN_PORT
YAML

# ---- start relay ----
"$MOQX_BIN" --config="$RELAY_CFG" >"$TMPDIR/relay.log" 2>&1 &
RELAY_PID=$!

# Wait for relay to be ready (admin /info endpoint)
for i in $(seq 1 20); do
  curl -sf "http://localhost:$ADMIN_PORT/info" >/dev/null 2>&1 && break
  sleep 0.5
done
curl -sf "http://localhost:$ADMIN_PORT/info" >/dev/null || {
  echo "ERROR: relay did not start. Log:"; cat "$TMPDIR/relay.log"; exit 1
}

# ---- start publisher ----
"$MOQBIN/moqswitchpub" \
  --relay_url="https://localhost:$RELAY_PORT/moq-relay" \
  --ns="test" --interval_ms=100 --insecure \
  >"$TMPDIR/pub.log" 2>&1 &
PUB_PID=$!
sleep 1  # allow publisher to connect and begin publishing

# ---- scenario 1: clean switch ----
echo "Running scenario 1: clean switch..."
SUB1_OUT="$TMPDIR/sub1.json"
timeout "$TIMEOUT" "$MOQBIN/moqswitchsub" \
  --relay_url="https://localhost:$RELAY_PORT/moq-relay" \
  --ns="test" --warm_up_groups=5 --lag_seconds=0 --collect_groups=10 \
  --insecure >"$SUB1_OUT" 2>/dev/null || true

RESULT1=$(grep '"event":"result"' "$SUB1_OUT" | tail -1 || true)
if [ -z "$RESULT1" ]; then
  echo "ERROR: scenario 1 produced no result line. Output:"; cat "$SUB1_OUT"; exit 1
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
  --insecure >"$SUB2_OUT" 2>/dev/null || true

RESULT2=$(grep '"event":"result"' "$SUB2_OUT" | tail -1 || true)
if [ -z "$RESULT2" ]; then
  echo "ERROR: scenario 2 produced no result line. Output:"; cat "$SUB2_OUT"; exit 1
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
