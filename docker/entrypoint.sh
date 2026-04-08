#!/bin/sh
# Generate relay config from env vars and exec moqx.
#
# To use a custom config, mount it at /etc/moqx/config.yaml:
#   docker run -v /path/to/config.yaml:/etc/moqx/config.yaml:ro ...
#
# Env vars (used only when no custom config is mounted):
#   MOQX_CERT       — path to TLS certificate PEM (required unless MOQX_INSECURE=true)
#   MOQX_KEY        — path to TLS private key PEM  (required unless MOQX_INSECURE=true)
#   MOQX_PORT       — UDP listen port (default: 4433)
#   MOQX_ADMIN_PORT — admin HTTP port (default: 8000)
#   MOQX_INSECURE   — use built-in dev cert (default: false)
#   MOQX_MAX_TRACKS — max cached tracks (default: 1000)
#   MOQX_MAX_GROUPS — max groups per track in cache (default: 100)
#   MOQX_BIND_ADDR  — listen address: "0.0.0.0" (IPv4, default) or "::" (IPv6/dual-stack)
#
# Env vars (always apply):
#   MOQX_LOG_LEVEL  — min log level: 0=INFO 1=WARNING 2=ERROR 3=FATAL (default: 0)
#   MOQX_VERBOSE    — verbose/debug level: 0=off, 1-4=increasing detail (default: 0)
set -e

# Map MOQX_* logging env vars to GLOG_* (used by folly/glog)
export GLOG_logtostderr=1
export GLOG_minloglevel="${MOQX_LOG_LEVEL:-0}"
export GLOG_v="${MOQX_VERBOSE:-0}"

# Enable core dumps (requires ulimits.core=-1 and --privileged in compose)
if [ -d /var/coredumps ] && [ -w /proc/sys/kernel/core_pattern ]; then
  echo "/var/coredumps/core.%e.%p.%t" > /proc/sys/kernel/core_pattern
fi

# Use custom config if mounted, otherwise generate from env vars
CONFIG=/etc/moqx/config.yaml
if [ -f "$CONFIG" ]; then
  echo "Using custom config: $CONFIG"
  exec /usr/local/bin/moqx --config "$CONFIG" "$@"
fi

CONFIG=/tmp/relay.yaml

cat > "$CONFIG" <<EOF
listeners:
  - name: relay
    udp:
      socket:
        address: "${MOQX_BIND_ADDR:-0.0.0.0}"
        port: ${MOQX_PORT:-4433}
    tls:
      cert_file: "${MOQX_CERT:-}"
      key_file: "${MOQX_KEY:-}"
      insecure: ${MOQX_INSECURE:-false}
    endpoint: "/moq-relay"

services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache:
      enabled: true
      max_tracks: ${MOQX_MAX_TRACKS:-1000}
      max_groups_per_track: ${MOQX_MAX_GROUPS:-100}

admin:
  port: ${MOQX_ADMIN_PORT:-8000}
  address: "${MOQX_BIND_ADDR:-0.0.0.0}"
  plaintext: true
EOF

exec /usr/local/bin/moqx --config "$CONFIG" "$@"
