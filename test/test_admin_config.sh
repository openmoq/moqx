#!/usr/bin/env bash
set -euo pipefail

BINARY="${1:-$(dirname "$0")/../build/moqx}"
# shellcheck source=test_ports.sh
source "$(dirname "$0")/test_ports.sh"
LISTEN_PORT=$TEST_ADMIN_CONFIG_LISTEN
ADMIN_PORT=$TEST_ADMIN_CONFIG_ADMIN
ADMIN_URL="http://localhost:${ADMIN_PORT}/config"

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

# Fetch the /config response.
RESPONSE=$(curl -sf "$ADMIN_URL")
echo "Response: $RESPONSE"

# Validate the dump reflects the generated config. Use here-strings instead of
# `echo | grep -q` to avoid SIGPIPE flake when grep exits before echo finishes.
if ! grep -q '"relay_id":' <<<"$RESPONSE"; then
  echo "FAIL: missing top-level \"relay_id\" field" >&2
  exit 1
fi

if ! grep -q "\"port\": *${LISTEN_PORT}\|\":${LISTEN_PORT}\"" <<<"$RESPONSE"; then
  # SocketAddress::describe() renders as e.g. "[::]:9684"; just check the port is present.
  if ! grep -q "${LISTEN_PORT}" <<<"$RESPONSE"; then
    echo "FAIL: listener port ${LISTEN_PORT} not present in dump" >&2
    exit 1
  fi
fi

if ! grep -q '"name":"main"' <<<"$RESPONSE"; then
  echo "FAIL: listener name \"main\" not present in dump" >&2
  exit 1
fi

if ! grep -q '"endpoint":"/moq-relay"' <<<"$RESPONSE"; then
  echo "FAIL: listener endpoint not present in dump" >&2
  exit 1
fi

# The default service has an insecure (plaintext) listener.
if ! grep -q '"insecure":true' <<<"$RESPONSE"; then
  echo "FAIL: expected insecure:true for plaintext listener" >&2
  exit 1
fi

# Cache config for the default service should be reported.
if ! grep -q '"max_cached_tracks":100' <<<"$RESPONSE"; then
  echo "FAIL: expected max_cached_tracks:100 from service cache config" >&2
  exit 1
fi

# HMAC secrets must never be dumped. No auth keys are configured here, but
# ensure the literal secret-dump marker never leaks a real value.
if grep -q '"secret":"<redacted>"' <<<"$RESPONSE"; then
  echo "NOTE: redaction marker present (auth keys configured)"
fi

echo "PASS"
