#!/usr/bin/env bash
set -euo pipefail

BINARY="${1:-$(dirname "$0")/../build/o_rly}"
TESTDIR="$(cd "$(dirname "$0")" && pwd)"
ADMIN_PORT=9671
LISTEN_PORT=9666

if [[ ! -x "$BINARY" ]]; then
  echo "ERROR: binary not found or not executable: $BINARY" >&2
  exit 1
fi

CERT="${TESTDIR}/test_cert.pem"
KEY="${TESTDIR}/test_key.pem"

TMPDIR=$(mktemp -d)
O_RLY_PID=""
cleanup() {
  [[ -n "$O_RLY_PID" ]] && kill "$O_RLY_PID" 2>/dev/null; wait "$O_RLY_PID" 2>/dev/null || true
  rm -rf "$TMPDIR"
}
trap cleanup EXIT

cat > "$TMPDIR/config.yaml" <<EOF
listeners:
  - name: main
    udp:
      socket:
        address: "::"
        port: ${LISTEN_PORT}
    tls:
      insecure: true
    endpoint: "/moq-relay"
cache:
  enabled: true
  max_tracks: 100
  max_groups_per_track: 3
admin:
  port: ${ADMIN_PORT}
  tls:
    cert_file: ${CERT}
    key_file: ${KEY}
    insecure: false
EOF

ADMIN_URL="https://localhost:${ADMIN_PORT}/info"

"$BINARY" --config="$TMPDIR/config.yaml" &
O_RLY_PID=$!

# Wait up to 5s for the admin server to become ready.
for i in $(seq 1 50); do
  if curl -sf -k "$ADMIN_URL" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
  if [[ $i -eq 50 ]]; then
    echo "ERROR: admin server did not become ready in time" >&2
    exit 1
  fi
done

check_response() {
  local label="$1"
  local response="$2"
  if ! echo "$response" | grep -q '"service":"o-rly"'; then
    echo "FAIL [${label}]: missing \"service\":\"o-rly\" in response" >&2
    exit 1
  fi
  echo "PASS [${label}]"
}

RESPONSE=$(curl -sf -k --http2 "$ADMIN_URL")
check_response "h2" "$RESPONSE"

RESPONSE=$(curl -sf -k --http1.1 "$ADMIN_URL")
check_response "http/1.1" "$RESPONSE"

RESPONSE=$(curl -sf -k --no-alpn "$ADMIN_URL")
check_response "no-alpn" "$RESPONSE"
