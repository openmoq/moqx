#!/usr/bin/env bash
# make_test_config.sh — write a minimal moqx test config to stdout.
#
# Usage: make_test_config.sh <listen_port> <admin_port> [--cert <file> --key <file>]
#                            [--tmpdir <dir>]
#
# Without --cert/--key the admin section uses plaintext: true.
# With --cert/--key the admin section uses plaintext: false and includes TLS config.
#
# --tmpdir gives the listener's generated cert somewhere to live; it is only
# consulted when MOQ_HARNESS_QUIC_STACK selects picoquic.

set -euo pipefail

source "$(dirname "$0")/test_versions.sh"
source "$(dirname "$0")/test_quic_stack.sh"

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <listen_port> <admin_port> [--cert <file> --key <file>]" >&2
  exit 1
fi

LISTEN_PORT="$1"
ADMIN_PORT="$2"
shift 2

CERT=""
KEY=""
STACK_TMPDIR=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --cert) CERT="$2"; shift 2 ;;
    --key)  KEY="$2";  shift 2 ;;
    --tmpdir) STACK_TMPDIR="$2"; shift 2 ;;
    *) echo "Unknown argument: $1" >&2; exit 1 ;;
  esac
done

cat <<EOF
listeners:
  - name: main
    udp:
      socket:
        address: "::"
        port: ${LISTEN_PORT}
$(moq_listener_stack_yaml "$STACK_TMPDIR")
    endpoint: "/moq-relay"
    moqt_versions: ${MOQT_TEST_VERSIONS}
services:
  default:
    match:
      - authority: {any: true}
$(moq_service_path_yaml)
    cache:
      enabled: true
      max_tracks: 100
      max_groups_per_track: 3
admin:
  port: ${ADMIN_PORT}
  address: "::1"
  track_metrics_enabled: true
EOF

if [[ -n "$CERT" ]]; then
  cat <<EOF
  plaintext: false
  tls:
    cert_file: ${CERT}
    key_file: ${KEY}
EOF
else
  echo "  plaintext: true"
fi
