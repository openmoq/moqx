#!/bin/bash
# Test that relay pair shuts down cleanly (no ASAN leaks or crashes).
# Usage: ./scripts/test-relay-shutdown.sh [up_config] [down_config]
#   up_config   config with no upstream (default: /tmp/up.yaml, port 12345)
#   down_config config with upstream pointing to up (default: /tmp/down.yaml, port 12346)

set -euo pipefail

BIN="${BIN:-./build-san/moqx}"
UP_CFG="${1:-/tmp/up.yaml}"
DOWN_CFG="${2:-/tmp/down.yaml}"
SHUTDOWN_TIMEOUT="${SHUTDOWN_TIMEOUT:-10}"  # seconds to wait for clean exit before declaring a hang

if [[ ! -x "$BIN" ]]; then
  echo "ERROR: binary not found: $BIN (set BIN= to override)" >&2
  exit 1
fi

UP_LOG=$(mktemp /tmp/moqx-up-XXXXXX.log)
DOWN_LOG=$(mktemp /tmp/moqx-down-XXXXXX.log)
PASS=true

cleanup() {
  kill "$UP_PID" "$DOWN_PID" 2>/dev/null || true
  wait "$UP_PID" "$DOWN_PID" 2>/dev/null || true
  rm -f "$UP_LOG" "$DOWN_LOG"
}
trap cleanup EXIT

# wait_or_kill <pid> <label> <exit_var>
# Waits up to $SHUTDOWN_TIMEOUT seconds for <pid> to exit, then SIGKILL and fail.
wait_or_kill() {
  local pid="$1" label="$2" exitvar="$3"
  local deadline=$(( $(date +%s) + SHUTDOWN_TIMEOUT ))
  while kill -0 "$pid" 2>/dev/null; do
    if (( $(date +%s) >= deadline )); then
      echo "HANG: $label did not exit within ${SHUTDOWN_TIMEOUT}s — sending SIGKILL" >&2
      kill -KILL "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
      eval "$exitvar=1"
      PASS=false
      return
    fi
    sleep 0.5
  done
  local wrc=0
  wait "$pid" 2>/dev/null && wrc=0 || wrc=$?
  eval "$exitvar=$wrc"
}

echo "==> Starting upstream relay (no upstream provider)..."
"$BIN" --config "$UP_CFG" --logging=DBG4 >"$UP_LOG" 2>&1 &
UP_PID=$!

echo "==> Starting downstream relay (has upstream provider)..."
"$BIN" --config "$DOWN_CFG" --logging=DBG4 >"$DOWN_LOG" 2>&1 &
DOWN_PID=$!

echo "==> Waiting for downstream to connect to upstream..."
for i in $(seq 1 20); do
  if grep -q "reconnectLoop connected" "$DOWN_LOG" 2>/dev/null; then
    echo "    Connected after ${i}s."
    break
  fi
  sleep 1
  if ! kill -0 "$DOWN_PID" 2>/dev/null; then
    echo "ERROR: downstream relay died unexpectedly" >&2
    cat "$DOWN_LOG" >&2
    exit 1
  fi
done

if ! grep -q "reconnectLoop connected" "$DOWN_LOG" 2>/dev/null; then
  echo "ERROR: downstream never connected" >&2
  cat "$DOWN_LOG" >&2
  exit 1
fi

# --- Scenario 1: kill downstream (has active upstream session) first ---
echo ""
echo "==> [Scenario 1] Killing downstream relay (active upstream session)..."
kill -INT "$DOWN_PID"
DOWN_EXIT=0
wait_or_kill "$DOWN_PID" "downstream" DOWN_EXIT
echo "    Downstream exited with code $DOWN_EXIT."

if grep -q "LeakSanitizer\|ERROR: AddressSanitizer\|Aborted\|Check failed" "$DOWN_LOG"; then
  echo "FAIL: downstream had ASAN errors or crash:"
  grep -E "LeakSanitizer|ERROR: AddressSanitizer|Aborted|Check failed|SUMMARY:" "$DOWN_LOG" | head -20
  PASS=false
else
  echo "    PASS: no ASAN errors in downstream."
fi

echo "==> Waiting 3s then killing upstream relay..."
sleep 3
kill -INT "$UP_PID"
UP_EXIT=0
wait_or_kill "$UP_PID" "upstream" UP_EXIT
echo "    Upstream exited with code $UP_EXIT."

if grep -q "LeakSanitizer\|ERROR: AddressSanitizer\|Aborted\|Check failed" "$UP_LOG"; then
  echo "FAIL: upstream had ASAN errors or crash:"
  grep -E "LeakSanitizer|ERROR: AddressSanitizer|Aborted|Check failed|SUMMARY:" "$UP_LOG" | head -20
  PASS=false
else
  echo "    PASS: no ASAN errors in upstream."
fi

# --- Scenario 2: kill upstream first, wait for downstream to enter reconnect, then kill downstream ---
echo ""
echo "==> [Scenario 2] Restarting both relays..."
"$BIN" --config "$UP_CFG" --logging=DBG1 >>"$UP_LOG" 2>&1 &
UP_PID=$!
"$BIN" --config "$DOWN_CFG" --logging=DBG1 >>"$DOWN_LOG" 2>&1 &
DOWN_PID=$!

echo "==> Waiting for downstream to connect again..."
for i in $(seq 1 20); do
  if grep -c "reconnectLoop connected" "$DOWN_LOG" 2>/dev/null | grep -q "^[2-9]"; then
    echo "    Reconnected after ${i}s."
    break
  fi
  sleep 1
done

echo "==> Killing upstream first..."
kill -INT "$UP_PID"
UP_EXIT=0; wait_or_kill "$UP_PID" "upstream(s2)" UP_EXIT

echo "==> Waiting 4s for downstream to enter reconnect loop..."
sleep 4

echo "==> Killing downstream (in reconnect/backoff)..."
kill -INT "$DOWN_PID"
DOWN_EXIT=0
wait_or_kill "$DOWN_PID" "downstream(s2)" DOWN_EXIT
echo "    Downstream exited with code $DOWN_EXIT."

if grep -q "LeakSanitizer\|ERROR: AddressSanitizer\|Aborted\|Check failed" "$DOWN_LOG"; then
  echo "FAIL: downstream had ASAN errors or crash (scenario 2):"
  grep -E "LeakSanitizer|ERROR: AddressSanitizer|Aborted|Check failed|SUMMARY:" "$DOWN_LOG" | tail -30
  PASS=false
else
  echo "    PASS: no ASAN errors in downstream (scenario 2)."
fi

echo ""
if $PASS; then
  echo "ALL PASS"
else
  echo "SOME TESTS FAILED"
  exit 1
fi
