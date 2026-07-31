#!/usr/bin/env bash
set -euo pipefail

BINARY="${1:-$(dirname "$0")/../build/moqx}"
# shellcheck source=test_ports.sh
source "$(dirname "$0")/test_ports.sh"
LISTEN_PORT=$TEST_ADMIN_TRACK_METRICS_LISTEN
ADMIN_PORT=$TEST_ADMIN_TRACK_METRICS_ADMIN
TRACK_URL="http://localhost:${ADMIN_PORT}/metrics/track"
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

"$(dirname "$0")/make_test_config.sh" "$LISTEN_PORT" "$ADMIN_PORT" > "$TMPDIR/config.yaml"

"$BINARY" --config="$TMPDIR/config.yaml" &
MOQX_PID=$!

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

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

# A missing namespace parameter is a client error, not an empty scrape.
HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null "$TRACK_URL" 2>/dev/null)
[[ "$HTTP_CODE" == "400" ]] || fail "expected HTTP 400 without namespace, got $HTTP_CODE"

# No live tracks: a valid scrape that reports nothing, so Prometheus sees an
# empty result rather than an error.
HEADERS_FILE="$TMPDIR/headers.txt"
BODY_FILE="$TMPDIR/body.txt"
HTTP_CODE=$(curl -sw "%{http_code}" -D "$HEADERS_FILE" -o "$BODY_FILE" \
  "${TRACK_URL}?namespace=test-ns" 2>/dev/null)
[[ "$HTTP_CODE" == "200" ]] || fail "expected HTTP 200 for empty match, got $HTTP_CODE"

if ! grep -qi 'content-type:.*text/plain.*version=0\.0\.4' < "$HEADERS_FILE"; then
  echo "Headers: $(cat "$HEADERS_FILE")" >&2
  fail "missing or incorrect Content-Type header"
fi

# The metric families are declared even with no tracks, so a scrape of an idle
# namespace still describes the schema.
for metric in \
  moqx_track_objects_received_total \
  moqx_track_bytes_received_total \
  moqx_track_objects_sent_total \
  moqx_track_bytes_sent_total \
  moqx_track_datagrams_received_total \
  moqx_track_subgroups_received_total \
  moqx_track_groups_received_total \
  moqx_track_subscribers \
  moqx_track_publish_start_timestamp_seconds \
  moqx_track_last_object_timestamp_seconds; do
  grep -q "^# TYPE ${metric} " < "$BODY_FILE" || fail "missing TYPE line for ${metric}"
done

# Filters and limits are accepted.
HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null \
  "${TRACK_URL}?namespace=test-ns&track=track1&service=default&limit=5" 2>/dev/null)
[[ "$HTTP_CODE" == "200" ]] || fail "expected HTTP 200 for filtered query, got $HTTP_CODE"

HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null "${TRACK_URL}?namespace=test-ns&limit=0" 2>/dev/null)
[[ "$HTTP_CODE" == "400" ]] || fail "expected HTTP 400 for limit=0, got $HTTP_CODE"

HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null "${TRACK_URL}?namespace=test-ns&limit=abc" 2>/dev/null)
[[ "$HTTP_CODE" == "400" ]] || fail "expected HTTP 400 for non-numeric limit, got $HTTP_CODE"

# Names are in the MoQT safe form, so raw bytes outside it are a client error
# rather than a silently different query.
HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null "${TRACK_URL}?namespace=test%2Fns" 2>/dev/null)
[[ "$HTTP_CODE" == "400" ]] || fail "expected HTTP 400 for unencoded '/', got $HTTP_CODE"

HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null "${TRACK_URL}?namespace=test-ns&track=a%20b" 2>/dev/null)
[[ "$HTTP_CODE" == "400" ]] || fail "expected HTTP 400 for unencoded space in track, got $HTTP_CODE"

# .2f is '/', so this is the encoded form of a one-tuple namespace "test/ns".
HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null "${TRACK_URL}?namespace=test.2fns" 2>/dev/null)
[[ "$HTTP_CODE" == "200" ]] || fail "expected HTTP 200 for encoded namespace, got $HTTP_CODE"

# Disabled: the endpoint must say so rather than return an empty scrape that
# reads as "no live tracks".
kill "$MOQX_PID" 2>/dev/null || true
wait "$MOQX_PID" 2>/dev/null || true
MOQX_PID=""

python3 - "$TMPDIR/config.yaml" <<'PYEOF'
import sys
path = sys.argv[1]
text = open(path).read()
open(path, "w").write(text.replace("admin:\n", "admin:\n  track_metrics_enabled: false\n", 1))
PYEOF

"$BINARY" --config="$TMPDIR/config.yaml" &
MOQX_PID=$!
for i in $(seq 1 100); do
  HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null "$INFO_URL" 2>/dev/null || echo "000")
  [[ "$HTTP_CODE" == "200" ]] && break
  sleep 0.1
  [[ $i -eq 100 ]] && fail "relay with track metrics disabled did not become ready"
done

HTTP_CODE=$(curl -sw "%{http_code}" -o "$BODY_FILE" "${TRACK_URL}?namespace=test-ns" 2>/dev/null)
[[ "$HTTP_CODE" == "503" ]] || fail "expected HTTP 503 when disabled, got $HTTP_CODE"
grep -q "track_metrics_enabled" < "$BODY_FILE" || fail "503 body should name the config knob"

echo "PASS: /metrics/track endpoint"
