#!/bin/sh
# Generate relay config from env vars and exec o_rly.
#
# Env vars:
#   ORLY_CERT      — path to TLS certificate PEM (required unless ORLY_INSECURE=true)
#   ORLY_KEY       — path to TLS private key PEM  (required unless ORLY_INSECURE=true)
#   ORLY_PORT      — UDP listen port (default: 4433)
#   ORLY_INSECURE  — use built-in dev cert (default: false)
set -e

CONFIG=/tmp/relay.yaml

cat > "$CONFIG" <<EOF
listeners:
  - name: relay
    udp:
      socket:
        address: "::"
        port: ${ORLY_PORT:-4433}
    tls:
      cert_file: "${ORLY_CERT:-}"
      key_file: "${ORLY_KEY:-}"
      insecure: ${ORLY_INSECURE:-false}
    endpoint: "/moq-relay"

cache:
  enabled: true
  max_tracks: 100
  max_groups_per_track: 3

admin:
  port: 9669
  address: "::"
  plaintext: true
EOF

exec /usr/local/bin/o-rly --config "$CONFIG" "$@"
