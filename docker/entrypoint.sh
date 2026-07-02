#!/bin/sh
# Generate relay config from env vars and exec moqx.
#
# Resolves docker/config.docker.yaml (a ${MOQX_*} template) with envsubst,
# unless a full config is mounted at /etc/moqx/config.yaml:
#   docker run -v /path/to/config.yaml:/etc/moqx/config.yaml:ro ...
#
# Connection / TLS:
#   MOQX_CERT       — TLS certificate PEM (required unless MOQX_INSECURE=true)
#   MOQX_KEY        — TLS private key PEM  (required unless MOQX_INSECURE=true)
#   MOQX_PORT       — mvfst UDP listen port (default: 4433)
#   MOQX_PICO_ENABLE— also run a picoquic listener (default: true; auto-off when
#                     MOQX_INSECURE=true, since picoquic requires real TLS)
#   MOQX_PICO_PORT  — picoquic UDP listen port (default: 4434)
#   MOQX_ADMIN_PORT — admin HTTP port (default: 8000)
#   MOQX_INSECURE   — use built-in dev cert (default: false)
#   MOQX_BIND_ADDR  — listen address: 0.0.0.0 (default) or :: (dual-stack)
#   MOQX_ENDPOINT   — WebTransport endpoint path (default: /moq-relay)
#   MOQX_MOQT_VERSIONS — advertised MoQT draft versions, YAML inline list
#                     (default: [16, 14, 18]). The relay selects by server
#                     preference — the first listed version the client also
#                     supports — so draft 18 listed last is a fallback: only
#                     d18-only clients negotiate 18; clients that also speak
#                     14/16 settle on 16. Reorder to change preference.
#   MOQX_MAX_TRACKS — max cached tracks (default: 1000)
#   MOQX_MAX_GROUPS — max groups per track in cache (default: 100)
#   MOQX_CACHE      — relay object cache enabled (default: true)
#
# Performance (inherited by the CI deploy / interop adapter; override to tune):
#   MOQX_THREADS          — IO worker threads (default: 4; must be >= 1)
#   MOQX_LOCAL_FWD        — per-subscriber-thread local forwarders (default: true)
#   MOQX_SEND_PKTS        — mvfst max_conn_packets_sent_per_loop (default: 16)
#   MOQX_RECV_PKTS        — mvfst max_server_recv_packets_per_loop (default: 256)
#   MOQX_UDP_BUFFER       — relay UDP socket buffer bytes (default: net.core.wmem_max)
#   MOQX_IGNORE_PATH_MTU  — send full-size packets, skip PMTU (default: false)
#   MOQX_JEMALLOC         — LD_PRELOAD jemalloc (~10% speedup). "auto" (default)
#                           probes the multiarch paths; off/false/0 uses the
#                           system allocator; an explicit path forces that lib.
#
# Logging (always apply):
#   MOQX_LOG_LEVEL  — min log level: 0=INFO 1=WARNING 2=ERROR 3=FATAL (default: 0)
#   MOQX_VERBOSE    — verbose/debug level: 0=off, 1-4=increasing detail (default: 0)
set -e

# Map MOQX_* logging env vars to GLOG_* (used by folly/glog)
export GLOG_logtostderr=1
export GLOG_minloglevel="${MOQX_LOG_LEVEL:-0}"
export GLOG_v="${MOQX_VERBOSE:-0}"

# Issuer mode: a short-lived utility sub-entrypoint, dispatched before any
# relay setup (jemalloc/config) so it stays unaffected by it.
if [ "${1:-}" = "issue-cat-token" ]; then
  shift
  exec /usr/local/bin/moqx-issuer "$@"
fi

# jemalloc: LD_PRELOAD by default (~10% relay speedup). Resolved here so both
# exec paths (custom config + rendered template) inherit LD_PRELOAD. Opt out
# with MOQX_JEMALLOC in {off,false,0,no}; an explicit path forces that lib.
case "${MOQX_JEMALLOC:-auto}" in
  off|false|0|no) ;;   # system allocator
  auto|on|true|1|yes|"")
    for _jelib in \
        /usr/lib/x86_64-linux-gnu/libjemalloc.so.2 \
        /usr/lib/aarch64-linux-gnu/libjemalloc.so.2 \
        /usr/lib/libjemalloc.so.2 \
        /usr/local/lib/libjemalloc.so.2; do
      [ -f "$_jelib" ] && { export LD_PRELOAD="${_jelib}${LD_PRELOAD:+:$LD_PRELOAD}"; break; }
    done
    if [ -n "${LD_PRELOAD:-}" ]; then
      echo "note: jemalloc enabled (LD_PRELOAD=$LD_PRELOAD)" >&2
    else
      echo "warning: jemalloc requested but libjemalloc.so.2 not found; using system allocator" >&2
    fi ;;
  *)
    if [ -f "$MOQX_JEMALLOC" ]; then
      export LD_PRELOAD="${MOQX_JEMALLOC}${LD_PRELOAD:+:$LD_PRELOAD}"
      echo "note: jemalloc enabled (LD_PRELOAD=$MOQX_JEMALLOC)" >&2
    else
      echo "warning: jemalloc path not found: $MOQX_JEMALLOC; using system allocator" >&2
    fi ;;
esac

# Enable core dumps (requires ulimits.core=-1 and --privileged in compose)
if [ -d /var/coredumps ] && [ -w /proc/sys/kernel/core_pattern ]; then
  echo "/var/coredumps/core.%e.%p.%t" > /proc/sys/kernel/core_pattern
fi

# Use custom config if mounted, otherwise render the template from env vars.
CONFIG=/etc/moqx/config.yaml
if [ -f "$CONFIG" ]; then
  echo "Using custom config: $CONFIG"
  exec /usr/local/bin/moqx --config "$CONFIG" "$@"
fi

# Defaults for every placeholder in config.docker.yaml (override via env).
export MOQX_BIND_ADDR="${MOQX_BIND_ADDR:-0.0.0.0}"
export MOQX_PORT="${MOQX_PORT:-4433}"
export MOQX_ADMIN_PORT="${MOQX_ADMIN_PORT:-8000}"
export MOQX_CERT="${MOQX_CERT:-}"
export MOQX_KEY="${MOQX_KEY:-}"
export MOQX_INSECURE="${MOQX_INSECURE:-false}"
export MOQX_ENDPOINT="${MOQX_ENDPOINT:-/moq-relay}"
export MOQX_MOQT_VERSIONS="${MOQX_MOQT_VERSIONS:-[16, 14, 18]}"
export MOQX_MAX_TRACKS="${MOQX_MAX_TRACKS:-1000}"
export MOQX_MAX_GROUPS="${MOQX_MAX_GROUPS:-100}"
export MOQX_CACHE="${MOQX_CACHE:-true}"
# Performance defaults (see header). MOQX_THREADS must be >= 1 (no autodetect).
export MOQX_THREADS="${MOQX_THREADS:-4}"
export MOQX_LOCAL_FWD="${MOQX_LOCAL_FWD:-true}"
export MOQX_SEND_PKTS="${MOQX_SEND_PKTS:-16}"
export MOQX_RECV_PKTS="${MOQX_RECV_PKTS:-256}"
export MOQX_UDP_BUFFER="${MOQX_UDP_BUFFER:-$(cat /proc/sys/net/core/wmem_max 2>/dev/null || echo 1048576)}"
export MOQX_IGNORE_PATH_MTU="${MOQX_IGNORE_PATH_MTU:-false}"

# Second (picoquic) listener for dual-stack serving. Opt out with
# MOQX_PICO_ENABLE=false. picoquic needs real TLS, so it is auto-disabled under
# MOQX_INSECURE=true. The block is injected into config.docker.yaml's
# ${MOQX_PICO_LISTENER} placeholder (empty when disabled). Values are expanded
# here by the shell, so envsubst substitutes the block verbatim.
export MOQX_PICO_ENABLE="${MOQX_PICO_ENABLE:-true}"
export MOQX_PICO_PORT="${MOQX_PICO_PORT:-4434}"
if [ "$MOQX_PICO_ENABLE" = "true" ] && [ "$MOQX_INSECURE" = "true" ]; then
  echo "note: picoquic listener disabled — it requires real TLS (MOQX_INSECURE=true)" >&2
  MOQX_PICO_ENABLE=false
fi
if [ "$MOQX_PICO_ENABLE" = "true" ]; then
  MOQX_PICO_LISTENER=$(cat <<PICO
  - name: relay-pico
    quic_stack: picoquic
    moqt_versions: ${MOQX_MOQT_VERSIONS}
    udp:
      socket:
        address: "${MOQX_BIND_ADDR}"
        port: ${MOQX_PICO_PORT}
    tls:
      cert_file: "${MOQX_CERT}"
      key_file: "${MOQX_KEY}"
      insecure: ${MOQX_INSECURE}
    endpoint: "${MOQX_ENDPOINT}"
PICO
)
else
  MOQX_PICO_LISTENER=""
fi
export MOQX_PICO_LISTENER

CONFIG=/tmp/relay.yaml
envsubst < /usr/local/share/moqx/config.docker.yaml > "$CONFIG"

exec /usr/local/bin/moqx --config "$CONFIG" "$@"
