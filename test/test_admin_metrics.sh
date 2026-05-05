#!/usr/bin/env bash
set -euo pipefail

BINARY="${1:-$(dirname "$0")/../build/moqx}"
# shellcheck source=test_ports.sh
source "$(dirname "$0")/test_ports.sh"
LISTEN_PORT=$TEST_ADMIN_METRICS_LISTEN
ADMIN_PORT=$TEST_ADMIN_METRICS_ADMIN
METRICS_URL="http://localhost:${ADMIN_PORT}/metrics"
INFO_URL="http://localhost:${ADMIN_PORT}/info"

if [[ ! -x "$BINARY" ]]; then
  echo "ERROR: binary not found or not executable: $BINARY" >&2
  exit 1
fi

TMPDIR=$(mktemp -d)
MOQX_PID=""
# Cleanup function that ensures the process exits and port is released.
cleanup() {
  if [[ -n "${MOQX_PID:-}" ]]; then
    kill "$MOQX_PID" 2>/dev/null || true
    # Wait for process to exit and ensure port is released
    wait "$MOQX_PID" 2>/dev/null || true
  fi
  rm -rf "$TMPDIR"
}
trap cleanup EXIT

"$(dirname "$0")/make_test_config.sh" "$LISTEN_PORT" "$ADMIN_PORT" > "$TMPDIR/config.yaml"

# Start moqx with the generated config in the background.
"$BINARY" --config="$TMPDIR/config.yaml" &
MOQX_PID=$!

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
head -20 <<<"$RESPONSE"

# Note on `grep -q <<<"$VAR"` (here-string) instead of `echo "$VAR" | grep -q`:
# grep -q exits as soon as it finds a match, closing the pipe; the upstream
# echo then gets SIGPIPE/EPIPE and the pipeline fails under `set -o pipefail`
# even though grep clearly succeeded. Here-strings have no pipe, no race.

# Validate Content-Type header (Prometheus text exposition format v0.0.4).
if ! grep -qi 'content-type:.*text/plain.*version=0\.0\.4' <<<"$HEADERS"; then
  echo "FAIL: missing or incorrect Content-Type header, expected 'text/plain; version=0.0.4'" >&2
  echo "Headers: $HEADERS" >&2
  exit 1
fi

# Validate Prometheus format: every metric family must have a # HELP and # TYPE line.
if ! grep -q '^# HELP ' <<<"$RESPONSE"; then
  echo "FAIL: no '# HELP' lines found in /metrics response" >&2
  exit 1
fi

if ! grep -q '^# TYPE ' <<<"$RESPONSE"; then
  echo "FAIL: no '# TYPE' lines found in /metrics response" >&2
  exit 1
fi

# Validate a representative set of expected metric names.
EXPECTED_METRICS=(
  "moqx_moqActiveSessions"
  "moqx_pubActiveSubscriptions"
  "moqx_pubActivePublishers"
  "moqx_pubSubscribeSuccess_total"
  "moqx_moqPublishSuccess_total"
  "moqx_moqSubscribeLatency_microseconds"
  "moqx_moqFetchLatency_microseconds"
)

for metric in "${EXPECTED_METRICS[@]}"; do
  if ! grep -q "$metric" <<<"$RESPONSE"; then
    echo "FAIL: expected metric '$metric' not found in /metrics response" >&2
    exit 1
  fi
done

# Validate histogram structure: bucket, sum, and count lines must be present.
if ! grep -q '_bucket{le=' <<<"$RESPONSE"; then
  echo "FAIL: no histogram bucket lines (e.g. '_bucket{le=...}') found" >&2
  exit 1
fi

if ! grep -q '_sum ' <<<"$RESPONSE"; then
  echo "FAIL: no histogram _sum lines found" >&2
  exit 1
fi

if ! grep -q '_count ' <<<"$RESPONSE"; then
  echo "FAIL: no histogram _count lines found" >&2
  exit 1
fi

echo "PASS"
