#!/usr/bin/env bash
set -euo pipefail

BINARY="${1:-$(dirname "$0")/../build/moqx}"
# shellcheck source=test_ports.sh
source "$(dirname "$0")/test_ports.sh"
LISTEN_PORT=$TEST_ADMIN_INFO_LISTEN
ADMIN_PORT=$TEST_ADMIN_INFO_ADMIN
ADMIN_URL="http://localhost:${ADMIN_PORT}/info"

if [[ ! -x "$BINARY" ]]; then
  echo "ERROR: binary not found or not executable: $BINARY" >&2
  exit 1
fi

TMPDIR=$(mktemp -d)
MOQX_PID=""
cleanup() {
  [[ -n "$MOQX_PID" ]] && kill "$MOQX_PID" 2>/dev/null; wait "$MOQX_PID" 2>/dev/null || true
  rm -rf "$TMPDIR"
}
trap cleanup EXIT

"$(dirname "$0")/make_test_config.sh" "$LISTEN_PORT" "$ADMIN_PORT" > "$TMPDIR/config.yaml"

# Start moqx with the generated config in the background.
"$BINARY" --config="$TMPDIR/config.yaml" &
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

# Fetch the /info response.
RESPONSE=$(curl -sf "$ADMIN_URL")
echo "Response: $RESPONSE"

# Validate expected fields. Use here-strings instead of `echo | grep -q` to
# avoid SIGPIPE flake when grep -q exits before echo finishes writing.
if ! grep -q '"service":"moqx"' <<<"$RESPONSE"; then
  echo "FAIL: missing \"service\":\"moqx\" in response" >&2
  exit 1
fi

if ! grep -q '"version":' <<<"$RESPONSE"; then
  echo "FAIL: missing \"version\" field in response" >&2
  exit 1
fi

echo "PASS"
