#!/usr/bin/env bash
# Run the moxygen conformance test suite against a moqx relay.
#
# Usage: test_conformance.sh <moqx_binary> [versions] [Q] [stack]
#
#   versions — "14", "16", or "14,16" (default: moxygen default)
#   Q        — literal "Q" to use raw QUIC transport (default: WebTransport)
#   stack    — "mvfst" (default) or "pico" — selects the relay's QUIC stack
#
# Args are positional but order-independent: each is identified by content.
#
# Environment:
#   MOQBIN — path to moxygen install bin/ (for moqtest_client, moqtest_server)
#            defaults to .scratch/moxygen-install/bin
#
# Examples:
#   test_conformance.sh ./build/moqx
#   test_conformance.sh ./build/moqx 16
#   test_conformance.sh ./build/moqx 14 Q          # mvfst, draft-14, raw QUIC
#   test_conformance.sh ./build/moqx 16 Q pico     # picoquic, draft-16, raw QUIC
#   test_conformance.sh ./build/moqx 14 pico       # picoquic, draft-14, WT

set -euo pipefail

MOQX_BIN="${1:?Usage: $0 <moqx_binary> [versions] [Q] [stack]}"
shift
EXTRA_ARGS=("$@")

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MOQBIN="${MOQBIN:-${PROJECT_ROOT}/.scratch/moxygen-install/bin}"
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

# Parse args. Each one is identified by content so order doesn't matter.
QUIC_STACK="mvfst"
DOWNSTREAM_ARGS=()
SERVER_VERSIONS_FLAG=()
SERVER_TRANSPORT_FLAG=()
for arg in "${EXTRA_ARGS[@]}"; do
  case "$arg" in
    mvfst|pico)
      QUIC_STACK="$arg"
      ;;
    Q)
      SERVER_TRANSPORT_FLAG=(--quic_transport=true)
      DOWNSTREAM_ARGS+=("$arg")
      ;;
    *)
      if [[ "$arg" =~ ^[0-9]+(,[0-9]+)*$ ]]; then
        SERVER_VERSIONS_FLAG=(--versions="$arg")
      fi
      DOWNSTREAM_ARGS+=("$arg")
      ;;
  esac
done

# Pick a random port range to avoid collisions with parallel runs
RELAY_PORT=$((19700 + RANDOM % 100))
ADMIN_PORT=$((RELAY_PORT + 1))

# picoquic dual-stack v6 bind doesn't currently receive v4-mapped packets
# (openmoq/moxygen#170). Bind v4 explicitly when stack=pico; mvfst is fine on "::".
# picoquic also requires real TLS credentials — insecure mode is unsupported
# (openmoq/moxygen#176). URL_HOST is the matching client-side address — must
# agree with the bind family or 'localhost' may resolve to ::1 and miss the
# v4-only listener (CI's /etc/hosts lists ::1 first).
TMPDIR=$(mktemp -d)
if [[ "$QUIC_STACK" = "pico" ]]; then
  BIND_ADDRESS="0.0.0.0"
  URL_HOST="127.0.0.1"
  STACK_LINE="    quic_stack: picoquic"
  # Generate ephemeral self-signed cert valid for localhost.
  openssl req -newkey rsa:2048 -nodes \
    -keyout "$TMPDIR/cert.key" -x509 -out "$TMPDIR/cert.pem" \
    -subj '/CN=conformance-test' -addext 'subjectAltName=DNS:localhost' \
    >/dev/null 2>&1
  TLS_BLOCK="    tls:
      insecure: false
      cert_file: \"$TMPDIR/cert.pem\"
      key_file: \"$TMPDIR/cert.key\""
else
  BIND_ADDRESS="::"
  URL_HOST="localhost"
  STACK_LINE=""
  TLS_BLOCK="    tls:
      insecure: true"
fi

# Generate a temp config with our ports
TMPCONFIG="$TMPDIR/config.yaml"
trap 'rm -rf "$TMPDIR"; kill "$RELAY_PID" "$SERVER_PID" 2>/dev/null; wait "$RELAY_PID" "$SERVER_PID" 2>/dev/null' EXIT

cat > "$TMPCONFIG" <<EOF
listeners:
  - name: conformance
${STACK_LINE}
    udp:
      socket:
        address: "${BIND_ADDRESS}"
        port: ${RELAY_PORT}
${TLS_BLOCK}
    endpoint: "/moq-relay"
services:
  default:
    match:
      - authority: {any: true}
        path: {exact: "/moq-relay"}
service_defaults:
  cache:
    enabled: true
    max_tracks: 1000
    max_groups_per_track: 100
admin:
  port: ${ADMIN_PORT}
  address: "::1"
  plaintext: true
EOF

echo "==> Starting moqx relay (stack=${QUIC_STACK}) on port ${RELAY_PORT}..."
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
# moqtest_server's --versions and --quic_transport must match the client's,
# otherwise the server-relay session and client-relay session land on
# different MoQ versions / transports and the relay can't bridge them
# (every client invocation then hangs to its 30s transaction timeout).
"$MOQBIN/moqtest_server" \
  --relay_url="https://${URL_HOST}:${RELAY_PORT}/moq-relay" \
  "${SERVER_VERSIONS_FLAG[@]}" \
  "${SERVER_TRANSPORT_FLAG[@]}" \
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
RELAY_URL="https://${URL_HOST}:${RELAY_PORT}/moq-relay"

# conformance_test.sh expects moqtest_client at $MOXYGEN_DIR/moxygen/moqtest/moqtest_client
# Create a symlink tree matching the expected layout
MOXYGEN_SHIM=$(mktemp -d)
mkdir -p "$MOXYGEN_SHIM/moxygen/moqtest"
ln -s "$MOQBIN/moqtest_client" "$MOXYGEN_SHIM/moxygen/moqtest/moqtest_client"
export MOXYGEN_DIR="$MOXYGEN_SHIM"
trap 'rm -rf "$MOXYGEN_SHIM" "$TMPDIR"; kill "$RELAY_PID" "$SERVER_PID" 2>/dev/null; wait "$RELAY_PID" "$SERVER_PID" 2>/dev/null' EXIT

set +e
bash "$CONFORMANCE_SCRIPT" "$RELAY_URL" "${DOWNSTREAM_ARGS[@]}"
EXIT_CODE=$?
set -e

echo "==> Conformance tests finished (exit code: $EXIT_CODE)"

# Clean up before exiting so background process kills don't affect exit code
kill "$RELAY_PID" "$SERVER_PID" 2>/dev/null
wait "$RELAY_PID" "$SERVER_PID" 2>/dev/null || true
rm -rf "$MOXYGEN_SHIM" "$TMPDIR"

# Disable the trap — we already cleaned up
trap - EXIT

exit $EXIT_CODE
