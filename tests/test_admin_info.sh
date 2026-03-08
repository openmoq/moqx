#!/usr/bin/env bash
set -euo pipefail

BINARY="${1:-$(dirname "$0")/../build/o_rly}"
ADMIN_PORT=9669
ADMIN_URL="http://localhost:${ADMIN_PORT}/info"

if [[ ! -x "$BINARY" ]]; then
  echo "ERROR: binary not found or not executable: $BINARY" >&2
  exit 1
fi

# Start o_rly in insecure mode (no TLS needed for this test) in the background.
"$BINARY" --insecure --admin_port="$ADMIN_PORT" &
O_RLY_PID=$!
trap 'kill "$O_RLY_PID" 2>/dev/null; wait "$O_RLY_PID" 2>/dev/null || true' EXIT

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

# Fetch the /info response.
RESPONSE=$(curl -sf "$ADMIN_URL")
echo "Response: $RESPONSE"

# Validate expected fields.
if ! echo "$RESPONSE" | grep -q '"service":"o-rly"'; then
  echo "FAIL: missing \"service\":\"o-rly\" in response" >&2
  exit 1
fi

if ! echo "$RESPONSE" | grep -q '"version":'; then
  echo "FAIL: missing \"version\" field in response" >&2
  exit 1
fi

echo "PASS"
