#!/usr/bin/env bash
set -euo pipefail

BINARY="${1:-$(dirname "$0")/../build/o_rly}"
TESTDIR="$(cd "$(dirname "$0")" && pwd)"
ADMIN_PORT=9669
METRICS_URL="http://localhost:${ADMIN_PORT}/metrics"
INFO_URL="http://localhost:${ADMIN_PORT}/info"

if [[ ! -x "$BINARY" ]]; then
  echo "ERROR: binary not found or not executable: $BINARY" >&2
  exit 1
fi

# Start o_rly with test config in the background.
"$BINARY" --config="$TESTDIR/test.config.yaml" &
O_RLY_PID=$!

# Cleanup function that ensures the process exits and port is released.
cleanup() {
  if [[ -n "${O_RLY_PID:-}" ]]; then
    kill "$O_RLY_PID" 2>/dev/null || true
    # Wait for process to exit and ensure port is released
    wait "$O_RLY_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

# Wait for the admin server to become ready (up to 10 s) using /info.
# /metrics is significantly more expensive, so avoid using it for readiness.
for i in $(seq 1 100); do
  HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null "$INFO_URL" 2>/dev/null || echo "000")
  if [[ "$HTTP_CODE" == "200" ]]; then
    break
  fi
  sleep 0.1
  if [[ $i -eq 100 ]]; then
    echo "ERROR: admin /info endpoint did not become ready in time (HTTP $HTTP_CODE)" >&2
    exit 1
  fi
done

# Fetch the /metrics response with headers in a single call to avoid timing issues.
# Use -D to save headers to a temp file, -w to capture response code.
HEADERS_FILE=$(mktemp)
trap 'rm -f "$HEADERS_FILE"' RETURN
HTTP_CODE=$(curl -sw "%{http_code}" -D "$HEADERS_FILE" -o /tmp/metrics_response.txt "$METRICS_URL" 2>/dev/null)

if [[ "$HTTP_CODE" != "200" ]]; then
  echo "FAIL: expected HTTP 200, got HTTP $HTTP_CODE" >&2
  cat "$HEADERS_FILE" >&2
  exit 1
fi

HEADERS=$(cat "$HEADERS_FILE")
RESPONSE=$(cat /tmp/metrics_response.txt)
rm -f /tmp/metrics_response.txt
echo "Response (first 20 lines):"
echo "$RESPONSE" | head -20

# Validate Content-Type header (Prometheus text exposition format v0.0.4).
if ! echo "$HEADERS" | grep -qi 'content-type:.*text/plain.*version=0\.0\.4'; then
  echo "FAIL: missing or incorrect Content-Type header, expected 'text/plain; version=0.0.4'" >&2
  echo "Headers: $HEADERS" >&2
  exit 1
fi

# Validate Prometheus format: every metric family must have a # HELP and # TYPE line.
if ! echo "$RESPONSE" | grep -q '^# HELP '; then
  echo "FAIL: no '# HELP' lines found in /metrics response" >&2
  exit 1
fi

if ! echo "$RESPONSE" | grep -q '^# TYPE '; then
  echo "FAIL: no '# TYPE' lines found in /metrics response" >&2
  exit 1
fi

# Validate a representative set of expected metric names.
EXPECTED_METRICS=(
  "orly_moqActiveSessions"
  "orly_moqActiveSubscriptions"
  "orly_moqActivePublishers"
  "orly_moqSubscribeSuccess_total"
  "orly_moqPublishSuccess_total"
  "orly_moqSubscribeLatency_microseconds"
  "orly_moqFetchLatency_microseconds"
)

for metric in "${EXPECTED_METRICS[@]}"; do
  if ! echo "$RESPONSE" | grep -q "$metric"; then
    echo "FAIL: expected metric '$metric' not found in /metrics response" >&2
    exit 1
  fi
done

# Validate histogram structure: bucket, sum, and count lines must be present.
if ! echo "$RESPONSE" | grep -q '_bucket{le='; then
  echo "FAIL: no histogram bucket lines (e.g. '_bucket{le=...}') found" >&2
  exit 1
fi

if ! echo "$RESPONSE" | grep -q '_sum '; then
  echo "FAIL: no histogram _sum lines found" >&2
  exit 1
fi

if ! echo "$RESPONSE" | grep -q '_count '; then
  echo "FAIL: no histogram _count lines found" >&2
  exit 1
fi

echo "PASS"
