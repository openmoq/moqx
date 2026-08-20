#!/usr/bin/env bash
set -euo pipefail

BINARY="${1:-$(dirname "$0")/../build/default/moqx}"
# shellcheck source=test_ports.sh
source "$(dirname "$0")/test_ports.sh"
# shellcheck source=test_relay_lifecycle.sh
source "$(dirname "$0")/test_relay_lifecycle.sh"
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
DATESERVER_PIDS=()
TEXTCLIENT_PIDS=()
cleanup() {
  local relay_failed=0
  for pid in "${TEXTCLIENT_PIDS[@]:-}" "${DATESERVER_PIDS[@]:-}"; do
    if [[ -n "$pid" ]]; then
      kill "$pid" 2>/dev/null || true
      reap_helpers "$pid"
    fi
  done
  if [[ -n "${MOQX_PID:-}" ]]; then
    kill "$MOQX_PID" 2>/dev/null || true
    reap_relays "$MOQX_PID" || relay_failed=1
  fi
  rm -rf "$TMPDIR"
  (( relay_failed == 0 )) || exit 1
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

# wait_sessions <min> <label>: wait for moqActiveSessions >= min.
wait_sessions() {
  local min="$1" label="$2"
  local deadline=$(( $(date +%s) + 10 ))
  local val
  until val=$(curl -sf "http://localhost:${ADMIN_PORT}/metrics" 2>/dev/null \
        | grep "^moqx_moqActiveSessions " | awk '{print $2}') \
        && [[ -n "$val" && "$val" -ge "$min" ]]; do
    (( $(date +%s) >= deadline )) \
      && fail "$label: moqActiveSessions=${val:-?} < $min after 10s"
    sleep 0.1
  done
}

# An omitted namespace scrapes every namespace, still bounded by limit.
HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null "$TRACK_URL" 2>/dev/null)
[[ "$HTTP_CODE" == "200" ]] || fail "expected HTTP 200 without namespace, got $HTTP_CODE"

HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null "${TRACK_URL}?limit=20" 2>/dev/null)
[[ "$HTTP_CODE" == "200" ]] || fail "expected HTTP 200 for limit-only scrape, got $HTTP_CODE"

# An explicitly empty namespace means the same thing as omitting it.
HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null "${TRACK_URL}?namespace=" 2>/dev/null)
[[ "$HTTP_CODE" == "200" ]] || fail "expected HTTP 200 for empty namespace, got $HTTP_CODE"

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

# omit_metadata drops the schema lines from both metrics endpoints.
for url in "${TRACK_URL}?namespace=test-ns&omit_metadata=1" \
           "http://localhost:${ADMIN_PORT}/metrics?omit_metadata"; do
  HTTP_CODE=$(curl -sw "%{http_code}" -o "$BODY_FILE" "$url" 2>/dev/null)
  [[ "$HTTP_CODE" == "200" ]] || fail "expected HTTP 200 for omit_metadata, got $HTTP_CODE"
  if grep -q '^# ' < "$BODY_FILE"; then
    fail "omit_metadata should drop HELP/TYPE lines: $url"
  fi
done
grep -q "^moqx_moqActiveSessions " < "$BODY_FILE" \
  || fail "omit_metadata should keep sample lines"

HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null \
  "${TRACK_URL}?namespace=test-ns&omit_metadata=yes" 2>/dev/null)
[[ "$HTTP_CODE" == "400" ]] || fail "expected HTTP 400 for non-boolean omit_metadata, got $HTTP_CODE"

HTTP_CODE=$(curl -sw "%{http_code}" -o "$BODY_FILE" \
  "${TRACK_URL}?namespace=test-ns&omit_metadata=false" 2>/dev/null)
[[ "$HTTP_CODE" == "200" ]] || fail "expected HTTP 200 for omit_metadata=false, got $HTTP_CODE"
grep -q "^# TYPE moqx_track_subscribers " < "$BODY_FILE" \
  || fail "omit_metadata=false should keep TYPE lines"

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

# A live track, so the counters and the all-namespaces scrape are exercised
# against real series rather than an empty body.
# shellcheck source=test_moqbin.sh
source "$(dirname "$0")/test_moqbin.sh"
resolve_moqbin "$BINARY"
DATESERVER="$MOQBIN/moqdateserver"
TEXTCLIENT="$MOQBIN/moqtextclient"
# No '-' or '.' in the namespace: both are metacharacters in the safe form, so
# this way the label is the namespace verbatim.
DATE_NS="moqdatetrackmetrics"
# A second live track, so a limit below the match count has something to reject.
DATE_NS2="moqdatetrackmetricstwo"

if [[ -x "$DATESERVER" && -x "$TEXTCLIENT" ]]; then
  for ns in "$DATE_NS" "$DATE_NS2"; do
    "$DATESERVER" \
      --relay_url="https://localhost:${LISTEN_PORT}/moq-relay" \
      --ns="$ns" --publish --insecure \
      >"$TMPDIR/dateserver-${ns}.log" 2>&1 &
    DATESERVER_PIDS+=("$!")
  done

  # Subscribing before the publisher has connected just fails.
  wait_sessions 2 "dateservers"

  # The relay sets forward=0 with no subscribers, so nothing is ingested to
  # count until someone is actually receiving the track.
  for ns in "$DATE_NS" "$DATE_NS2"; do
    "$TEXTCLIENT" \
      --connect_url="https://localhost:${LISTEN_PORT}/moq-relay" \
      --track_namespace="$ns" --track_name="date" --insecure \
      >"$TMPDIR/textclient-${ns}.log" 2>&1 &
    TEXTCLIENT_PIDS+=("$!")
  done
  wait_sessions 4 "textclients"

  # The date track emits about one object a second. Scraped without a namespace
  # parameter: this is the all-namespaces form.
  OBJECTS=0
  for i in $(seq 1 200); do
    curl -s -o "$BODY_FILE" "${TRACK_URL}?limit=20" 2>/dev/null || true
    # No match yet is expected while the track is coming up, and a failing grep
    # under pipefail would abort the script instead of retrying.
    OBJECTS=$(grep "^moqx_track_objects_received_total{.*namespace=\"${DATE_NS}\"" < "$BODY_FILE" \
      | head -1 | awk '{print $NF}' || true)
    if [[ -n "$OBJECTS" && "$OBJECTS" -gt 0 ]]; then
      break
    fi
    sleep 0.1
    if [[ $i -eq 200 ]]; then
      echo "--- scrape ---" >&2
      cat "$BODY_FILE" >&2
      echo "--- dateserver ---" >&2
      cat "$TMPDIR/dateserver-${DATE_NS}.log" >&2
      echo "--- textclient ---" >&2
      cat "$TMPDIR/textclient-${DATE_NS}.log" >&2
      fail "live track never reported a counted object in an all-namespaces scrape"
    fi
  done

  # Timestamps are Unix seconds carrying a millisecond fraction.
  grep -qE "^moqx_track_last_object_timestamp_seconds\{[^}]*\} [0-9]{10,}\.[0-9]{3}$" \
    < "$BODY_FILE" || fail "last_object timestamp is not seconds with a 3-digit fraction"

  # An explicit namespace selects the same track the wide scrape found.
  curl -s -o "$BODY_FILE" "${TRACK_URL}?namespace=${DATE_NS}" 2>/dev/null
  grep -q "namespace=\"${DATE_NS}\"" < "$BODY_FILE" \
    || fail "explicit namespace query did not return the live track"

  # limit bounds the scrape at the value asked for, not at the configured
  # default: below the match count is a 400, at it a 200.
  for i in $(seq 1 100); do
    curl -s -o "$BODY_FILE" "${TRACK_URL}?limit=20" 2>/dev/null || true
    TRACKS=$(grep -c "^moqx_track_subscribers{" < "$BODY_FILE" || true)
    [[ "$TRACKS" == "2" ]] && break
    sleep 0.1
    [[ $i -eq 100 ]] && fail "both live tracks never appeared in a scrape (saw $TRACKS)"
  done

  HTTP_CODE=$(curl -sw "%{http_code}" -o "$BODY_FILE" "${TRACK_URL}?limit=1" 2>/dev/null)
  [[ "$HTTP_CODE" == "400" ]] || fail "expected HTTP 400 for limit=1 with 2 live tracks, got $HTTP_CODE"
  grep -q "exceeds limit=1;" < "$BODY_FILE" \
    || fail "400 body should report the requested limit: $(cat "$BODY_FILE")"

  HTTP_CODE=$(curl -sw "%{http_code}" -o /dev/null "${TRACK_URL}?limit=2" 2>/dev/null)
  [[ "$HTTP_CODE" == "200" ]] || fail "expected HTTP 200 for limit=2 with 2 live tracks, got $HTTP_CODE"

  kill "${TEXTCLIENT_PIDS[@]}" "${DATESERVER_PIDS[@]}" 2>/dev/null || true
  reap_helpers "${TEXTCLIENT_PIDS[@]}" "${DATESERVER_PIDS[@]}"
else
  echo "SKIP: moqdateserver/moqtextclient not found in $MOQBIN; live-track checks skipped" >&2
fi

# Disabled: the endpoint must say so rather than return an empty scrape that
# reads as "no live tracks".
kill "$MOQX_PID" 2>/dev/null || true
reap_relays "$MOQX_PID" || fail "relay with track metrics enabled did not shut down cleanly"
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
