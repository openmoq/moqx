#!/usr/bin/env bash
# Exercises GET /state against a real relay: the route wiring, the walk across
# service executors, the producer and consumer coroutines, and the chunked
# framing, none of which the unit tests reach.
set -euo pipefail

BINARY="${1:-$(dirname "$0")/../build/default/moqx}"
# shellcheck source=test_ports.sh
source "$(dirname "$0")/test_ports.sh"
# shellcheck source=test_relay_lifecycle.sh
source "$(dirname "$0")/test_relay_lifecycle.sh"
LISTEN_PORT=$TEST_ADMIN_STATE_LISTEN
ADMIN_PORT=$TEST_ADMIN_STATE_ADMIN
ADMIN_URL="http://localhost:${ADMIN_PORT}/state"

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

"$(dirname "$0")/make_test_config.sh" "$LISTEN_PORT" "$ADMIN_PORT" > "$TMPDIR/config.yaml"

"$BINARY" --config="$TMPDIR/config.yaml" &
MOQX_PID=$!

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

curl -sf -D "$TMPDIR/headers.txt" -o "$TMPDIR/body.json" "$ADMIN_URL"
echo "Headers:"; cat "$TMPDIR/headers.txt"
echo "Body: $(cat "$TMPDIR/body.json")"

if ! grep -qi '^Transfer-Encoding: chunked' "$TMPDIR/headers.txt"; then
  echo "FAIL: /state is not chunked" >&2
  exit 1
fi

if grep -qi '^Content-Length:' "$TMPDIR/headers.txt"; then
  echo "FAIL: chunked response must not carry Content-Length" >&2
  exit 1
fi

if ! grep -qi '^Content-Type: application/json' "$TMPDIR/headers.txt"; then
  echo "FAIL: missing JSON content type" >&2
  exit 1
fi

# Reassembled chunks have to parse: every service's fragment is written by a
# different executor into one shared writer, and a flush that corrupted comma or
# nesting state would land here too.
python3 -c "
import json, sys
state = json.load(open('$TMPDIR/body.json'))
for key in ('relay_id', 'active_sessions', 'services'):
    if key not in state:
        sys.exit('FAIL: missing %s' % key)
if not state['services']:
    sys.exit('FAIL: no services in state')
for name, svc in state['services'].items():
    for key in ('downstream_peers', 'subscriptions', 'namespace_tree'):
        if key not in svc:
            sys.exit('FAIL: service %s missing %s' % (name, key))
    # These were removed: they counted per-thread forwarders, not subscribers.
    if 'subscribers' in svc or 'forwarding_subscribers' in svc:
        sys.exit('FAIL: service %s still reports subscriber counts' % name)
"

# Hanging up mid-response must not take the relay with it.
curl -s --max-time 0.001 "$ADMIN_URL" >/dev/null 2>&1 || true
if ! curl -sf "$ADMIN_URL" >/dev/null; then
  echo "FAIL: relay stopped serving /state after a client disconnect" >&2
  exit 1
fi

echo "PASS"
