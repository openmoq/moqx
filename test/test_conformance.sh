#!/usr/bin/env bash
# Run the moxygen conformance test suite against a moqx relay.
#
# Usage: test_conformance.sh <moqx_binary> [versions] [Q]
#
# Environment:
#   MOQBIN — path to moxygen install bin/ (for moqtest_client, moqtest_server)
#            defaults to .scratch/moxygen-install/bin
#
# Examples:
#   test_conformance.sh ./build/moqx
#   test_conformance.sh ./build/moqx 16
#   test_conformance.sh ./build/moqx 14 Q    # QUIC transport, draft-14
#   test_conformance.sh ./build/moqx 16 Q    # QUIC transport, draft-16

set -euo pipefail

MOQX_BIN="${1:?Usage: $0 <moqx_binary> [versions] [Q]}"
shift
EXTRA_ARGS=("$@")

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MOQBIN="${MOQBIN:-${PROJECT_ROOT}/.scratch/moxygen-install/bin}"
CONFIG="${PROJECT_ROOT}/test/test.config.yaml"
CONFORMANCE_SCRIPT="${PROJECT_ROOT}/deps/moxygen/moxygen/moqtest/conformance_test.sh"

# Validate binaries exist
for bin in "$MOQX_BIN" "$MOQBIN/moqtest_client" "$MOQBIN/moqtest_server"; do
  if [[ ! -x "$bin" ]]; then
    echo "Error: binary not found or not executable: $bin" >&2
    exit 1
  fi
done

if [[ ! -x "$CONFORMANCE_SCRIPT" ]]; then
  echo "Error: conformance script not found: $CONFORMANCE_SCRIPT" >&2
  exit 1
fi

# Pick a random port range to avoid collisions with parallel runs
RELAY_PORT=$((19700 + RANDOM % 100))
ADMIN_PORT=$((RELAY_PORT + 1))

# Generate a temp config with our ports
TMPCONFIG=$(mktemp)
trap 'rm -f "$TMPCONFIG"; kill "$RELAY_PID" "$SERVER_PID" 2>/dev/null; wait "$RELAY_PID" "$SERVER_PID" 2>/dev/null' EXIT

cat > "$TMPCONFIG" <<EOF
listeners:
  - name: conformance
    udp:
      socket:
        address: "::"
        port: ${RELAY_PORT}
    tls:
      insecure: true
    endpoint: "/moq-relay"
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache:
      enabled: true
      max_tracks: 1000
      max_groups_per_track: 100
admin:
  port: ${ADMIN_PORT}
  address: "::1"
  plaintext: true
EOF

echo "==> Starting moqx relay on port ${RELAY_PORT}..."
"$MOQX_BIN" --config "$TMPCONFIG" --logtostderr &
RELAY_PID=$!

# Wait for admin endpoint
for i in $(seq 1 40); do
  if curl -sf "http://[::1]:${ADMIN_PORT}/info" >/dev/null 2>&1; then
    break
  fi
  sleep 0.25
  if [[ "$i" -eq 40 ]]; then
    echo "Error: relay did not start within 10s" >&2
    exit 1
  fi
done
echo "==> Relay ready (PID=${RELAY_PID})"

echo "==> Starting moqtest_server..."
# moqtest_server's --versions must match the client's --versions, otherwise
# server-relay ALPN negotiates draft-16 (default) while client-relay
# negotiates draft-14, and the relay can't bridge across versions →
# every client invocation hangs to its connect/transaction timeout.
SERVER_VERSIONS_FLAG=()
for arg in "${EXTRA_ARGS[@]}"; do
  if [[ "$arg" =~ ^[0-9]+(,[0-9]+)*$ ]]; then
    SERVER_VERSIONS_FLAG=(--versions="$arg")
    break
  fi
done

"$MOQBIN/moqtest_server" \
  --relay_url="https://localhost:${RELAY_PORT}/moq-relay" \
  "${SERVER_VERSIONS_FLAG[@]}" \
  --logtostderr &
SERVER_PID=$!
sleep 2

# Verify server connected
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
  echo "Error: moqtest_server exited prematurely" >&2
  exit 1
fi
echo "==> moqtest_server ready (PID=${SERVER_PID})"

echo "==> Running conformance tests..."
RELAY_URL="https://localhost:${RELAY_PORT}/moq-relay"

# conformance_test.sh expects moqtest_client at $MOXYGEN_DIR/moxygen/moqtest/moqtest_client
# Create a symlink tree matching the expected layout
MOXYGEN_SHIM=$(mktemp -d)
mkdir -p "$MOXYGEN_SHIM/moxygen/moqtest"
ln -s "$MOQBIN/moqtest_client" "$MOXYGEN_SHIM/moxygen/moqtest/moqtest_client"
export MOXYGEN_DIR="$MOXYGEN_SHIM"
trap 'rm -rf "$MOXYGEN_SHIM" "$TMPCONFIG"; kill "$RELAY_PID" "$SERVER_PID" 2>/dev/null; wait "$RELAY_PID" "$SERVER_PID" 2>/dev/null' EXIT

set +e
bash "$CONFORMANCE_SCRIPT" "$RELAY_URL" "${EXTRA_ARGS[@]}"
EXIT_CODE=$?
set -e

echo "==> Conformance tests finished (exit code: $EXIT_CODE)"

# Clean up before exiting so background process kills don't affect exit code
kill "$RELAY_PID" "$SERVER_PID" 2>/dev/null
wait "$RELAY_PID" "$SERVER_PID" 2>/dev/null || true
rm -rf "$MOXYGEN_SHIM" "$TMPCONFIG"

# Disable the trap — we already cleaned up
trap - EXIT

exit $EXIT_CODE
