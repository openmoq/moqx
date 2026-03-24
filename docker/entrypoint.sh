#!/bin/sh
# Generate relay config from env vars and exec o_rly.
#
# Env vars:
#   ORLY_CERT       — path to TLS certificate PEM (required unless ORLY_INSECURE=true)
#   ORLY_KEY        — path to TLS private key PEM  (required unless ORLY_INSECURE=true)
#   ORLY_PORT       — UDP listen port (default: 4433)
#   ORLY_ADMIN_PORT — admin HTTP port (default: 8000)
#   ORLY_INSECURE   — use built-in dev cert (default: false)
#   ORLY_MAX_TRACKS — max cached tracks (default: 1000)
#   ORLY_MAX_GROUPS — max groups per track in cache (default: 100)
#   ORLY_LOG_LEVEL  — min log level: 0=INFO 1=WARNING 2=ERROR 3=FATAL (default: 0)
#   ORLY_VERBOSE    — verbose/debug level: 0=off, 1-4=increasing detail (default: 0)
#   ORLY_BIND_ADDR  — listen address: "0.0.0.0" (IPv4, default) or "::" (IPv6/dual-stack)
set -e

# Map ORLY_* logging env vars to GLOG_* (used by folly/glog)
export GLOG_logtostderr=1
export GLOG_minloglevel="${ORLY_LOG_LEVEL:-0}"
export GLOG_v="${ORLY_VERBOSE:-0}"

# Enable core dumps (requires ulimits.core=-1 in compose)
if [ -d /var/coredumps ]; then
  echo "/var/coredumps/core.%e.%p.%t" > /proc/sys/kernel/core_pattern 2>/dev/null || true
fi

CONFIG=/tmp/relay.yaml

cat > "$CONFIG" <<EOF
listeners:
  - name: relay
    udp:
      socket:
        address: "${ORLY_BIND_ADDR:-0.0.0.0}"
        port: ${ORLY_PORT:-4433}
    tls:
      cert_file: "${ORLY_CERT:-}"
      key_file: "${ORLY_KEY:-}"
      insecure: ${ORLY_INSECURE:-false}
    endpoint: "/moq-relay"

cache:
  enabled: true
  max_tracks: ${ORLY_MAX_TRACKS:-1000}
  max_groups_per_track: ${ORLY_MAX_GROUPS:-100}

admin:
  port: ${ORLY_ADMIN_PORT:-8000}
  address: "::"
  plaintext: true
EOF

exec /usr/local/bin/o-rly --config "$CONFIG" "$@"
