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
#            defaults to the moxygen install bin (auto-detected from the build)
#
# Examples:
#   test_conformance.sh ./build/default/moqx
#   test_conformance.sh ./build/default/moqx 16
#   test_conformance.sh ./build/default/moqx 14 Q          # mvfst, draft-14, raw QUIC
#   test_conformance.sh ./build/default/moqx 16 Q pico     # picoquic, draft-16, raw QUIC
#   test_conformance.sh ./build/default/moqx 14 pico       # picoquic, draft-14, WT

set -euo pipefail

MOQX_BIN="${1:?Usage: $0 <moqx_binary> [versions] [Q] [stack]}"
shift
EXTRA_ARGS=("$@")

# `|| true`: a bad path must reach the friendly binary-not-found check below,
# not abort here under errexit.
BUILD_DIR="$(cd "$(dirname "$MOQX_BIN")" 2>/dev/null && pwd || true)"

# shellcheck source=test_moqbin.sh
source "$(dirname "${BASH_SOURCE[0]}")/test_moqbin.sh"
resolve_moqbin "$MOQX_BIN"
# shellcheck source=test_relay_lifecycle.sh
source "$(dirname "${BASH_SOURCE[0]}")/test_relay_lifecycle.sh"

# shellcheck source=test_quic_stack.sh
source "$(dirname "${BASH_SOURCE[0]}")/test_quic_stack.sh"

# moxygen conformance script: MOXYGEN_SRC override, else the moxygen source this
# build resolved — the cache records it wherever it lives, including a local
# checkout.
CONFORMANCE_SCRIPT="${MOXYGEN_SRC:+${MOXYGEN_SRC}/moxygen/moqtest/conformance_test.sh}"
if [[ -z "$CONFORMANCE_SCRIPT" || ! -x "$CONFORMANCE_SCRIPT" ]]; then
  MOXYGEN_SRC_DIR="$(sed -n 's|^CPM_PACKAGE_moxygen_SOURCE_DIR:INTERNAL=||p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null || true)"
  CONFORMANCE_SCRIPT="${MOXYGEN_SRC_DIR:-${BUILD_DIR}/_deps/moxygen-src}/moxygen/moqtest/conformance_test.sh"
fi

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
# A positional stack name overrides MOQ_HARNESS_QUIC_STACK, which is how the
# rest of the suite selects its stack.
QUIC_STACK="$MOQ_QUIC_STACK"
DOWNSTREAM_ARGS=()
SERVER_VERSIONS_FLAG=()
SERVER_TRANSPORT_FLAG=()
for arg in "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"; do
  case "$arg" in
    mvfst)
      QUIC_STACK="mvfst"
      ;;
    pico|picoquic)
      QUIC_STACK="picoquic"
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

TMPDIR=$(mktemp -d)
BIND_ADDRESS="::"
URL_HOST="localhost"
# The positional argument may have overridden the environment, so re-point the
# shared helpers at whatever won.
MOQ_QUIC_STACK="$QUIC_STACK"
LISTENER_STACK_BLOCK="$(moq_listener_stack_yaml "$TMPDIR")"

# Generate a temp config with our ports
TMPCONFIG="$TMPDIR/config.yaml"
RELAY_PID=""
SERVER_PID=""
MOXYGEN_SHIM=""
cleanup() {
  local relay_failed=0
  if [[ -n "$SERVER_PID" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
    reap_helpers "$SERVER_PID"
  fi
  if [[ -n "$RELAY_PID" ]]; then
    kill "$RELAY_PID" 2>/dev/null || true
    reap_relays "$RELAY_PID" || relay_failed=1
  fi
  rm -rf "$TMPDIR"
  if [[ -n "$MOXYGEN_SHIM" ]]; then
    rm -rf "$MOXYGEN_SHIM"
  fi
  (( relay_failed == 0 )) || exit 1
}
trap cleanup EXIT

cat > "$TMPCONFIG" <<EOF
listeners:
  - name: conformance
    udp:
      socket:
        address: "${BIND_ADDRESS}"
        port: ${RELAY_PORT}
${LISTENER_STACK_BLOCK}
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
  "${SERVER_VERSIONS_FLAG[@]+"${SERVER_VERSIONS_FLAG[@]}"}" \
  "${SERVER_TRANSPORT_FLAG[@]+"${SERVER_TRANSPORT_FLAG[@]}"}" \
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

set +e
bash "$CONFORMANCE_SCRIPT" "$RELAY_URL" "${DOWNSTREAM_ARGS[@]+"${DOWNSTREAM_ARGS[@]}"}"
EXIT_CODE=$?
set -e

echo "==> Conformance tests finished (exit code: $EXIT_CODE)"

# Clean up before exiting so background process kills don't affect exit code
kill "$RELAY_PID" "$SERVER_PID" 2>/dev/null || true
reap_helpers "$SERVER_PID"
reap_relays "$RELAY_PID" || EXIT_CODE=1
rm -rf "$MOXYGEN_SHIM" "$TMPDIR"

# Disable the trap — we already cleaned up
trap - EXIT

exit $EXIT_CODE
