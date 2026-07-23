#!/usr/bin/env bash
set -euo pipefail

BINARY="${1:-$(dirname "$0")/../build/moqx}"
# shellcheck source=test_ports.sh
source "$(dirname "$0")/test_ports.sh"
LISTEN_PORT=$TEST_ADMIN_CONNECTION_LOGS_LISTEN
ADMIN_PORT=$TEST_ADMIN_CONNECTION_LOGS_ADMIN
LOGS_URL="http://localhost:${ADMIN_PORT}/logs"
INFO_URL="http://localhost:${ADMIN_PORT}/info"

if [[ ! -x "$BINARY" ]]; then
  echo "ERROR: binary not found or not executable: $BINARY" >&2
  exit 1
fi

TMPDIR=$(mktemp -d)
MOQX_PID=""
cleanup() {
  if [[ -n "${MOQX_PID:-}" ]]; then
    kill "$MOQX_PID" 2>/dev/null || true
    wait "$MOQX_PID" 2>/dev/null || true
  fi
  rm -rf "$TMPDIR"
}
trap cleanup EXIT

# Set up fake log directories
mkdir -p "$TMPDIR/mlog"
mkdir -p "$TMPDIR/qlog"

# Add logging config to the test config
"$(dirname "$0")/make_test_config.sh" "$LISTEN_PORT" "$ADMIN_PORT" > "$TMPDIR/config.yaml"
cat <<EOF >> "$TMPDIR/config.yaml"
logging:
  mlog:
    dir: "$TMPDIR/mlog"
  qlog:
    dir: "$TMPDIR/qlog"
EOF

# Create a fake mlog file
echo '{"fake":"mlog"}' > "$TMPDIR/mlog/abcdef123456.mlog"

# Start moqx with the generated config in the background.
"$BINARY" --config="$TMPDIR/config.yaml" &
MOQX_PID=$!

# Wait for readiness
for i in $(seq 1 100); do
  HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null "$INFO_URL" 2>/dev/null || echo "000")
  if [[ "$HTTP_CODE" == "200" ]]; then
    break
  fi
  sleep 0.1
  if [[ $i -eq 100 ]]; then
    echo "ERROR: admin /info endpoint did not become ready in time" >&2
    exit 1
  fi
done

echo "Running tests..."

# Test 1: Missing params returns 400
HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null "${LOGS_URL}" 2>/dev/null || true)
if [[ "$HTTP_CODE" != "400" ]]; then
  echo "FAIL: expected HTTP 400 for missing params, got $HTTP_CODE" >&2
  exit 1
fi

# Test 2: Invalid type returns 400
HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null "${LOGS_URL}?type=invalid&connection_id=123" 2>/dev/null || true)
if [[ "$HTTP_CODE" != "400" ]]; then
  echo "FAIL: expected HTTP 400 for invalid type, got $HTTP_CODE" >&2
  exit 1
fi

# Test 3: Valid type but missing file returns 404
HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null "${LOGS_URL}?type=mlog&connection_id=abcd" 2>/dev/null || true)
if [[ "$HTTP_CODE" != "404" ]]; then
  echo "FAIL: expected HTTP 404 for missing file, got $HTTP_CODE" >&2
  exit 1
fi

# Test 4: Valid file returns 200 and correct content
HEADERS_FILE=$(mktemp)
trap 'rm -f "$HEADERS_FILE"' RETURN
HTTP_CODE=$(curl -sw "%{http_code}" -D "$HEADERS_FILE" -o /tmp/logs_response.txt "${LOGS_URL}?type=mlog&connection_id=abcdef123456" 2>/dev/null || true)

if [[ "$HTTP_CODE" != "200" ]]; then
  echo "FAIL: expected HTTP 200 for existing mlog, got $HTTP_CODE" >&2
  exit 1
fi

HEADERS=$(cat "$HEADERS_FILE")
RESPONSE=$(cat /tmp/logs_response.txt)
rm -f /tmp/logs_response.txt

if ! grep -qi 'content-type:.*application/json' <<<"$HEADERS"; then
  echo "FAIL: expected application/json content type" >&2
  echo "Got headers: $HEADERS" >&2
  exit 1
fi

if ! grep -q '{"fake":"mlog"}' <<<"$RESPONSE"; then
  echo "FAIL: response body did not match expected mlog content" >&2
  exit 1
fi

echo "PASS"
