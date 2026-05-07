#!/usr/bin/env bash
set -euo pipefail

BINARY="${1:-$(dirname "$0")/../build/moqx}"
# shellcheck source=test_ports.sh
source "$(dirname "$0")/test_ports.sh"
LISTEN_PORT=$TEST_CACHE_PURGE_LISTEN
ADMIN_PORT=$TEST_CACHE_PURGE_ADMIN
INFO_URL="http://localhost:${ADMIN_PORT}/info"
PURGE_URL="http://localhost:${ADMIN_PORT}/cache/purge"

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

"$(dirname "$0")/make_test_config.sh" "$LISTEN_PORT" "$ADMIN_PORT" > "$TMPDIR/config.yaml"

"$BINARY" --config="$TMPDIR/config.yaml" &
MOQX_PID=$!

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

# Test 1: POST /cache/purge with empty JSON body purges all services.
RESPONSE=$(curl -sf -X POST "$PURGE_URL" -H 'Content-Type: application/json' -d '{}')
echo "POST /cache/purge {}: $RESPONSE"
if ! echo "$RESPONSE" | grep -q '"evicted":'; then
  echo "FAIL: missing evicted field in response" >&2
  exit 1
fi

# Test 2: POST /cache/purge with service targets a single named service.
RESPONSE=$(curl -sf -X POST "$PURGE_URL" -H 'Content-Type: application/json' -d '{"service":"default"}')
echo "POST /cache/purge {service:default}: $RESPONSE"
if ! echo "$RESPONSE" | grep -q '"evicted":'; then
  echo "FAIL: missing evicted field for named service purge" >&2
  exit 1
fi

# Test 3: POST /cache/purge with unknown service returns evicted:0.
RESPONSE=$(curl -sf -X POST "$PURGE_URL" -H 'Content-Type: application/json' -d '{"service":"nonexistent"}')
echo "POST /cache/purge {service:nonexistent}: $RESPONSE"
if ! echo "$RESPONSE" | grep -q '"evicted":0'; then
  echo "FAIL: expected evicted:0 for unknown service, got: $RESPONSE" >&2
  exit 1
fi

# Test 4: GET /cache/purge returns 404 (route only accepts POST).
HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null -X GET "$PURGE_URL" 2>/dev/null)
echo "GET /cache/purge: HTTP $HTTP_CODE"
if [[ "$HTTP_CODE" != "404" ]]; then
  echo "FAIL: expected 404 for GET /cache/purge, got $HTTP_CODE" >&2
  exit 1
fi

# Test 5: POST /cache/purge with invalid JSON returns 400.
HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null -X POST "$PURGE_URL" \
  -H 'Content-Type: application/json' -d 'not-json' 2>/dev/null)
echo "POST /cache/purge invalid JSON: HTTP $HTTP_CODE"
if [[ "$HTTP_CODE" != "400" ]]; then
  echo "FAIL: expected 400 for invalid JSON, got $HTTP_CODE" >&2
  exit 1
fi

# Test 6: POST /cache/purge with namespace only (no track) returns 200.
# No tracks are cached so we expect evicted:0.
RESPONSE=$(curl -sf -X POST "$PURGE_URL" -H 'Content-Type: application/json' \
  -d '{"namespace":"live/example"}')
echo "POST /cache/purge {namespace:live/example}: $RESPONSE"
if ! echo "$RESPONSE" | grep -q '"evicted":'; then
  echo "FAIL: missing evicted field for namespace-only purge" >&2
  exit 1
fi

# Test 7: namespace-only purge scoped to a specific service also returns 200.
RESPONSE=$(curl -sf -X POST "$PURGE_URL" -H 'Content-Type: application/json' \
  -d '{"service":"default","namespace":"live/example"}')
echo "POST /cache/purge {service:default,namespace:live/example}: $RESPONSE"
if ! echo "$RESPONSE" | grep -q '"evicted":'; then
  echo "FAIL: missing evicted field for service+namespace purge" >&2
  exit 1
fi

# Test 8: namespace+track (full track name) still works.
RESPONSE=$(curl -sf -X POST "$PURGE_URL" -H 'Content-Type: application/json' \
  -d '{"namespace":"live/example","track":"video"}')
echo "POST /cache/purge {namespace:live/example,track:video}: $RESPONSE"
if ! echo "$RESPONSE" | grep -q '"evicted":'; then
  echo "FAIL: missing evicted field for namespace+track purge" >&2
  exit 1
fi

echo "PASS"
