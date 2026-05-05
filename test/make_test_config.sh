#!/usr/bin/env bash
# make_test_config.sh — write a minimal moqx test config to stdout.
#
# Usage: make_test_config.sh <listen_port> <admin_port> [--cert <file> --key <file>]
#
# Without --cert/--key the admin section uses plaintext: true.
# With --cert/--key the admin section uses plaintext: false and includes TLS config.

set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <listen_port> <admin_port> [--cert <file> --key <file>]" >&2
  exit 1
fi

LISTEN_PORT="$1"
ADMIN_PORT="$2"
shift 2

CERT=""
KEY=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --cert) CERT="$2"; shift 2 ;;
    --key)  KEY="$2";  shift 2 ;;
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
      max_tracks: 100
      max_groups_per_track: 3
admin:
  port: ${ADMIN_PORT}
  address: "::1"
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
