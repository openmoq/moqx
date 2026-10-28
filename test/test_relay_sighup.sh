#!/usr/bin/env bash
# SIGHUP must leave the relay running: it logs a warning and keeps serving.
set -euo pipefail

BINARY="${1:-$(dirname "$0")/../build/default/moqx}"
# shellcheck source=test_ports.sh
source "$(dirname "$0")/test_ports.sh"
# shellcheck source=test_relay_lifecycle.sh
source "$(dirname "$0")/test_relay_lifecycle.sh"
LISTEN_PORT=$TEST_RELAY_SIGHUP_LISTEN
ADMIN_PORT=$TEST_RELAY_SIGHUP_ADMIN
ADMIN_URL="http://localhost:${ADMIN_PORT}/info"

if [[ ! -x "$BINARY" ]]; then
  echo "ERROR: binary not found or not executable: $BINARY" >&2
  exit 1
fi

TMPDIR=$(mktemp -d)
MOQX_PID=""
cleanup() {
  local relay_failed=0
  if [[ -n "$MOQX_PID" ]]; then
    kill "$MOQX_PID" 2>/dev/null || true
    reap_relays "$MOQX_PID" || relay_failed=1
  fi
  rm -rf "$TMPDIR"
  (( relay_failed == 0 )) || exit 1
}
trap cleanup EXIT

"$(dirname "$0")/make_test_config.sh" "$LISTEN_PORT" "$ADMIN_PORT" --tmpdir "$TMPDIR" > "$TMPDIR/config.yaml"

"$BINARY" --config="$TMPDIR/config.yaml" 2>"$TMPDIR/moqx.log" &
MOQX_PID=$!

# Wait for the admin server to become ready (up to 5 s).
for i in $(seq 1 50); do
  if curl -sf "$ADMIN_URL" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
  if [[ $i -eq 50 ]]; then
    echo "ERROR: admin server did not become ready in time" >&2
    exit 1
  fi
done

kill -HUP "$MOQX_PID"

# Wait for the relay to log the SIGHUP (up to 5 s).
for i in $(seq 1 50); do
  if grep -q 'Ignoring SIGHUP' "$TMPDIR/moqx.log"; then
    break
  fi
  if ! kill -0 "$MOQX_PID" 2>/dev/null; then
    echo "FAIL: relay exited on SIGHUP" >&2
    exit 1
  fi
  sleep 0.1
  if [[ $i -eq 50 ]]; then
    echo "FAIL: relay did not log the SIGHUP" >&2
    exit 1
  fi
done

# Catches a relay that logs SIGHUP and then shuts down.
sleep 1
if ! kill -0 "$MOQX_PID" 2>/dev/null; then
  echo "FAIL: relay exited after SIGHUP" >&2
  exit 1
fi
if ! curl -sf "$ADMIN_URL" >/dev/null; then
  echo "FAIL: admin server stopped responding after SIGHUP" >&2
  exit 1
fi

echo "PASS"
