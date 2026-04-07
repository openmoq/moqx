#!/usr/bin/env bash
set -euo pipefail

BINARY="${1:-$(dirname "$0")/../build/moqx}"
TESTDIR="$(cd "$(dirname "$0")" && pwd)"
ADMIN_PORT=9669
INFO_URL="http://localhost:${ADMIN_PORT}/info"
PURGE_URL="http://localhost:${ADMIN_PORT}/cache-purge"

if [[ ! -x "$BINARY" ]]; then
  echo "ERROR: binary not found or not executable: $BINARY" >&2
  exit 1
fi

"$BINARY" --config="$TESTDIR/test.config.yaml" &
MOQX_PID=$!

cleanup() {
  if [[ -n "${MOQX_PID:-}" ]]; then
    kill "$MOQX_PID" 2>/dev/null || true
    wait "$MOQX_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

# Wait for the admin server to become ready (up to 10 s).
for i in $(seq 1 100); do
  HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null "$INFO_URL" 2>/dev/null || echo "000")
  if [[ "$HTTP_CODE" == "200" ]]; then
    break
  fi
  sleep 0.1
  if [[ $i -eq 100 ]]; then
    echo "ERROR: admin server did not become ready in time (HTTP $HTTP_CODE)" >&2
    exit 1
  fi
done

# Test 1: POST /cache-purge with no query param purges all services.
RESPONSE=$(curl -sf -X POST "$PURGE_URL")
echo "POST /cache-purge: $RESPONSE"
if ! echo "$RESPONSE" | grep -q '"status":"ok"'; then
  echo "FAIL: missing status:ok in response" >&2
  exit 1
fi
if ! echo "$RESPONSE" | grep -q '"cleared":'; then
  echo "FAIL: missing cleared field in response" >&2
  exit 1
fi

# Test 2: POST /cache-purge?service=default targets the single named service.
RESPONSE=$(curl -sf -X POST "${PURGE_URL}?service=default")
echo "POST /cache-purge?service=default: $RESPONSE"
if ! echo "$RESPONSE" | grep -q '"cleared":1'; then
  echo "FAIL: expected cleared:1 for named service purge" >&2
  exit 1
fi

# Test 3: POST /cache-purge?service=nonexistent returns 404.
HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null -X POST "${PURGE_URL}?service=nonexistent" 2>/dev/null)
echo "POST /cache-purge?service=nonexistent: HTTP $HTTP_CODE"
if [[ "$HTTP_CODE" != "404" ]]; then
  echo "FAIL: expected 404 for unknown service, got $HTTP_CODE" >&2
  exit 1
fi

# Test 4: GET /cache-purge returns 404 (route only accepts POST).
HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null -X GET "$PURGE_URL" 2>/dev/null)
echo "GET /cache-purge: HTTP $HTTP_CODE"
if [[ "$HTTP_CODE" != "404" ]]; then
  echo "FAIL: expected 404 for GET /cache-purge, got $HTTP_CODE" >&2
  exit 1
fi

echo "PASS"
