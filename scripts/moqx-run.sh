#!/usr/bin/env bash
# Local moqx CLI launcher for bench/scaling runs.
#
# Resolves the ${MOQX_*}/${DOMAIN} placeholders in a config template
# (default scripts/config.bench.yaml) from CLI flags > .env > built-in
# defaults, then serves the resolved config. Reads a local .env if present.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── CLI parsing ──────────────────────────────────────────────────────────
usage() {
  cat <<EOF
Usage: $(basename "$0") [options] [-- <extra args to moqx>]

Logging (override .env/defaults):
  -v, --verbose N           GLOG_v (0=off, 1-4 increasing detail)
  -L, --log-level N         GLOG_minloglevel (0=INFO 1=WARN 2=ERR 3=FATAL)
  -m, --vmodule SPEC        GLOG_vmodule (e.g. MoQSession=4,MoQForwarder=3)
  -x, --xlog SPEC           folly XLOG config (passed as --logging=SPEC)

Relay tuning (templated into the config; CLI > .env > default):
      --threads N           IO worker threads, must be >= 1 (default 4)
      --udp-buffer BYTES    relay UDP socket buffer; 0 = moxygen 1 MB default
                            (default: net.core.wmem_max)
      --recv-pkts N         mvfst max_server_recv_packets_per_loop (default 256)
      --send-pkts N         mvfst max_conn_packets_sent_per_loop (default 16)
      --cc ALGO             congestion control; bbr works on both stacks (default bbr)
      --local-forwarders / --no-local-forwarders   (default: on)
      --cache / --no-cache  relay object cache (default: on)
      --relay-thread / --no-relay-thread   dedicated relay exec thread (default: on)

Listener (templated into the config; CLI > .env > default):
      --quic-stack STACK    listener quic stack: mvfst|picoquic (default mvfst)
      --port N              UDP listen port (default 4433)
      --admin-port N        admin HTTP port (default 8000)
      --endpoint PATH       WebTransport endpoint path (default /moq-relay)
      --insecure            use a self-signed dev cert; ignore --cert/--key
      --cert PATH           TLS cert PEM (default: letsencrypt under DOMAIN)
      --key PATH            TLS key PEM  (default: letsencrypt under DOMAIN)

Targeting:
      --subcmd CMD          moqx subcommand (default: serve)
      --config FILE         config YAML template (default: scripts/config.bench.yaml)
      --env FILE            alternate .env file (default: scripts/.env if present)
      --bin FILE            moqx binary path (default: <project>/build/moqx)

Execution:
  -j, --jemalloc [PATH]     LD_PRELOAD jemalloc for the relay (~10% speedup).
                            Bare flag auto-detects libjemalloc.so.2.
      --check-sysctl        report current vs recommended UDP/network sysctls
                            (with the commands to raise them) and exit
      --no-sudo             run without sudo (cert files must be user-readable)
  -n, --dry-run             print resolved env + command, don't exec
  -h, --help                this help

Environment overrides (via .env or shell):
  MOQX_BIN, MOQX_CONFIG, MOQX_ENV_FILE, MOQX_SUBCMD, MOQX_USE_SUDO, DOMAIN
  MOQX_VERBOSE, MOQX_LOG_LEVEL, GLOG_vmodule, MOQX_JEMALLOC
  MOQX_THREADS, MOQX_UDP_BUFFER, MOQX_RECV_PKTS, MOQX_SEND_PKTS, MOQX_CC,
  MOQX_LOCAL_FWD, MOQX_CACHE, MOQX_BPF_STEERING, MOQX_IGNORE_PATH_MTU,
  MOQX_RELAY_THREAD, MOQX_STACK, MOQX_PORT, MOQX_ADMIN_PORT, MOQX_ENDPOINT,
  MOQX_INSECURE, MOQX_CERT, MOQX_KEY, MOQX_MAX_TRACKS, MOQX_MAX_GROUPS

Examples:
  $0                                       # serve with .env + defaults
  $0 --threads 8 --udp-buffer 16777216     # 8 IO threads, 16 MB socket buffer
  $0 --recv-pkts 256 -j                    # Alan's recv loop + jemalloc
  $0 --check-sysctl                        # audit kernel UDP buffers, then exit
  $0 --subcmd validate-config --no-sudo    # just validate the config
  $0 -n                                    # show what would run
EOF
}

# ── sysctl probe — report current vs recommended; print fix commands ───────
# Reports only; never changes anything. Recommended values target a
# high-fan-out UDP relay (the #1 source of silent packet drops).
check_sysctl() {
  local mode="${1:-report}"          # report = full table; warn = only if low
  local keys=(net.core.rmem_max net.core.wmem_max net.core.rmem_default \
              net.core.wmem_default net.core.netdev_max_backlog \
              net.core.optmem_max net.ipv4.udp_rmem_min net.ipv4.udp_wmem_min)
  local rec=(16777216 16777216 4194304 4194304 65536 1048576 262144 262144)
  local i key r cur status low_cmd=""
  [[ "$mode" == report ]] && printf '%-30s %-14s %-14s %s\n' SYSCTL CURRENT RECOMMENDED STATUS
  for i in "${!keys[@]}"; do
    key="${keys[$i]}"; r="${rec[$i]}"
    cur="$(sysctl -n "$key" 2>/dev/null || true)"
    if [[ -z "$cur" ]]; then
      status="n/a"
    elif [[ "$cur" =~ ^[0-9]+$ ]] && (( cur < r )); then
      status="LOW"; low_cmd+=" $key=$r"
    else
      status="ok"
    fi
    [[ "$mode" == report ]] && printf '%-30s %-14s %-14s %s\n' "$key" "${cur:-?}" "$r" "$status"
  done
  if [[ -n "$low_cmd" ]]; then
    [[ "$mode" == report ]] && echo
    echo "WARNING: UDP buffer/backlog sysctls below recommended for high fan-out." >&2
    echo "  raise (run yourself — NOT executed here):" >&2
    echo "    sudo sysctl -w$low_cmd" >&2
    echo "  persist: put those KEY=VALUE lines in /etc/sysctl.d/99-moqx.conf, then 'sudo sysctl --system'" >&2
    return 1
  fi
  [[ "$mode" == report ]] && echo "All recommended sysctls satisfied."
  return 0
}

# normalize a boolean-ish value to true/false (or "INVALID")
norm_bool() { case "${1,,}" in 1|true|yes|on) echo true ;; 0|false|no|off) echo false ;; *) echo INVALID ;; esac; }

CLI_VERBOSE="" CLI_LOG_LEVEL="" CLI_VMODULE="" CLI_XLOG=""
CLI_SUBCMD="" CLI_CONFIG="" CLI_ENV_FILE="" CLI_BIN=""
CLI_USE_SUDO=""   # "" = unset; "0"/"1" set
CLI_JEMALLOC=""   # "" = unset; "auto" or explicit path
CLI_THREADS="" CLI_UDP_BUFFER="" CLI_RECV_PKTS="" CLI_SEND_PKTS="" CLI_CC=""
CLI_LOCAL_FWD="" CLI_CACHE="" CLI_RELAY_THREAD="" CLI_INSECURE=""   # tri-state booleans
CLI_STACK="" CLI_PORT="" CLI_ADMIN_PORT="" CLI_ENDPOINT="" CLI_CERT="" CLI_KEY=""
CHECK_SYSCTL=0 DRY_RUN=0
PASSTHRU=()

while (($#)); do
  case "$1" in
    -v|--verbose)    CLI_VERBOSE="$2"; shift 2 ;;
    -L|--log-level)  CLI_LOG_LEVEL="$2"; shift 2 ;;
    -m|--vmodule)    CLI_VMODULE="$2"; shift 2 ;;
    -x|--xlog)       CLI_XLOG="$2"; shift 2 ;;
    --threads)       CLI_THREADS="$2"; shift 2 ;;
    --udp-buffer)    CLI_UDP_BUFFER="$2"; shift 2 ;;
    --recv-pkts)     CLI_RECV_PKTS="$2"; shift 2 ;;
    --send-pkts)     CLI_SEND_PKTS="$2"; shift 2 ;;
    --cc)            CLI_CC="$2"; shift 2 ;;
    --local-forwarders)    CLI_LOCAL_FWD=true;  shift ;;
    --no-local-forwarders) CLI_LOCAL_FWD=false; shift ;;
    --cache)         CLI_CACHE=true;  shift ;;
    --no-cache)      CLI_CACHE=false; shift ;;
    --relay-thread)    CLI_RELAY_THREAD=true;  shift ;;
    --no-relay-thread) CLI_RELAY_THREAD=false; shift ;;
    --quic-stack)    CLI_STACK="$2"; shift 2 ;;
    --port)          CLI_PORT="$2"; shift 2 ;;
    --admin-port)    CLI_ADMIN_PORT="$2"; shift 2 ;;
    --endpoint)      CLI_ENDPOINT="$2"; shift 2 ;;
    --insecure)      CLI_INSECURE=true; shift ;;
    --cert)          CLI_CERT="$2"; shift 2 ;;
    --key)           CLI_KEY="$2"; shift 2 ;;
    --subcmd)        CLI_SUBCMD="$2"; shift 2 ;;
    --config)        CLI_CONFIG="$2"; shift 2 ;;
    --env)           CLI_ENV_FILE="$2"; shift 2 ;;
    --bin)           CLI_BIN="$2"; shift 2 ;;
    -j|--jemalloc)
      # Optional arg: a following token that isn't another flag is a path.
      if [[ -n "${2:-}" && "$2" != -* ]]; then CLI_JEMALLOC="$2"; shift 2
      else CLI_JEMALLOC="auto"; shift; fi ;;
    --check-sysctl)  CHECK_SYSCTL=1; shift ;;
    --no-sudo)       CLI_USE_SUDO=0; shift ;;
    -n|--dry-run)    DRY_RUN=1; shift ;;
    -h|--help)       usage; exit 0 ;;
    --)              shift; PASSTHRU+=("$@"); break ;;
    *)               echo "unknown arg: $1" >&2; usage >&2; exit 2 ;;
  esac
done

# --check-sysctl: report and exit (no .env / binary needed)
if (( CHECK_SYSCTL )); then check_sysctl report; exit $?; fi

# ── Folly XLOG passthrough (moxygen uses folly::XLOG, ignores GLOG_*) ──────
[[ -n "$CLI_XLOG" ]] && PASSTHRU+=("--logging=$CLI_XLOG")

# ── Source local .env if present (set -a exports each KEY=VALUE) ───────────
ENV_FILE="${CLI_ENV_FILE:-${MOQX_ENV_FILE:-$SCRIPT_DIR/.env}}"
if [[ -f "$ENV_FILE" ]]; then
  set -a
  # shellcheck disable=SC1090
  source "$ENV_FILE"
  set +a
fi

# ── Paths (CLI > env > defaults) ─────────────────────────────────────────
MOQX_BIN="${CLI_BIN:-${MOQX_BIN:-$PROJECT_ROOT/build/moqx}}"
CONFIG_TEMPLATE="${CLI_CONFIG:-${MOQX_CONFIG:-$SCRIPT_DIR/config.bench.yaml}}"

[[ -x "$MOQX_BIN" ]]        || { echo "moqx binary not found: $MOQX_BIN" >&2; exit 1; }
[[ -f "$CONFIG_TEMPLATE" ]] || { echo "config not found: $CONFIG_TEMPLATE" >&2; exit 1; }
command -v envsubst >/dev/null || { echo "envsubst missing (apt install gettext-base)" >&2; exit 1; }

# ── Relay tuning knobs (CLI > .env/env > default), exported for envsubst ──
WMEM_MAX="$(cat /proc/sys/net/core/wmem_max 2>/dev/null || echo 1048576)"
export MOQX_THREADS="${CLI_THREADS:-${MOQX_THREADS:-4}}"
export MOQX_SEND_PKTS="${CLI_SEND_PKTS:-${MOQX_SEND_PKTS:-16}}"
export MOQX_RECV_PKTS="${CLI_RECV_PKTS:-${MOQX_RECV_PKTS:-256}}"
export MOQX_UDP_BUFFER="${CLI_UDP_BUFFER:-${MOQX_UDP_BUFFER:-$WMEM_MAX}}"
export MOQX_CC="${CLI_CC:-${MOQX_CC:-bbr}}"
MOQX_LOCAL_FWD="$(norm_bool "${CLI_LOCAL_FWD:-${MOQX_LOCAL_FWD:-true}}")"
MOQX_CACHE="$(norm_bool "${CLI_CACHE:-${MOQX_CACHE:-true}}")"
MOQX_RELAY_THREAD="$(norm_bool "${CLI_RELAY_THREAD:-${MOQX_RELAY_THREAD:-true}}")"
MOQX_INSECURE="$(norm_bool "${CLI_INSECURE:-${MOQX_INSECURE:-false}}")"
# bpf steering and ignore_path_mtu are experimental — env override only, no flag.
MOQX_BPF_STEERING="$(norm_bool "${MOQX_BPF_STEERING:-false}")"
MOQX_IGNORE_PATH_MTU="$(norm_bool "${MOQX_IGNORE_PATH_MTU:-false}")"
for b in MOQX_LOCAL_FWD MOQX_CACHE MOQX_RELAY_THREAD MOQX_INSECURE MOQX_BPF_STEERING MOQX_IGNORE_PATH_MTU; do
  [[ "${!b}" == INVALID ]] && { echo "invalid boolean for $b (want true/false)" >&2; exit 2; }
done
export MOQX_LOCAL_FWD MOQX_CACHE MOQX_RELAY_THREAD MOQX_INSECURE
export MOQX_BPF_STEERING MOQX_IGNORE_PATH_MTU

# ── Listener knobs (CLI > .env/env > default), exported for envsubst ──────
export MOQX_STACK="${CLI_STACK:-${MOQX_STACK:-mvfst}}"
case "$MOQX_STACK" in mvfst|picoquic) ;; *) echo "invalid --quic-stack: $MOQX_STACK (want mvfst|picoquic)" >&2; exit 2 ;; esac
export MOQX_PORT="${CLI_PORT:-${MOQX_PORT:-4433}}"
export MOQX_ADMIN_PORT="${CLI_ADMIN_PORT:-${MOQX_ADMIN_PORT:-8000}}"
export MOQX_ENDPOINT="${CLI_ENDPOINT:-${MOQX_ENDPOINT:-/moq-relay}}"
export MOQX_MAX_TRACKS="${MOQX_MAX_TRACKS:-1000}"
export MOQX_MAX_GROUPS="${MOQX_MAX_GROUPS:-100}"
# TLS: cert/key default to letsencrypt under DOMAIN; --insecure ignores them.
export MOQX_CERT="${CLI_CERT:-${MOQX_CERT:-/etc/letsencrypt/live/${DOMAIN:-}/fullchain.pem}}"
export MOQX_KEY="${CLI_KEY:-${MOQX_KEY:-/etc/letsencrypt/live/${DOMAIN:-}/privkey.pem}}"

# ── Resolve placeholders into a temp config ───────────────────────────────
RESOLVED_CONFIG=/tmp/moqx-resolved.yaml
envsubst < "$CONFIG_TEMPLATE" > "$RESOLVED_CONFIG"

# ── GLOG — map MOQX_* → GLOG_* (matches docker/entrypoint.sh convention) ─
export GLOG_logtostderr=1
export GLOG_colorlogtostderr=1
export GLOG_minloglevel="${CLI_LOG_LEVEL:-${MOQX_LOG_LEVEL:-0}}"
export GLOG_v="${CLI_VERBOSE:-${MOQX_VERBOSE:-0}}"
export GLOG_vmodule="${CLI_VMODULE:-${GLOG_vmodule:-MoqxRelay=3,MoQSession=3,MoQForwarder=3,MoqxCache=2}}"

# ── jemalloc resolution (CLI > env) ──────────────────────────────────────
JEMALLOC_REQ="${CLI_JEMALLOC:-${MOQX_JEMALLOC:-}}"
JEMALLOC=""
if [[ -n "$JEMALLOC_REQ" ]]; then
  case "$JEMALLOC_REQ" in
    auto|1|true|yes)
      for cand in \
        /usr/lib/x86_64-linux-gnu/libjemalloc.so.2 \
        /usr/lib/aarch64-linux-gnu/libjemalloc.so.2 \
        /lib64/libjemalloc.so.2 \
        /usr/lib64/libjemalloc.so.2 \
        /usr/local/lib/libjemalloc.so.2; do
        [[ -f "$cand" ]] && { JEMALLOC="$cand"; break; }
      done
      if [[ -z "$JEMALLOC" ]] && command -v ldconfig >/dev/null 2>&1; then
        JEMALLOC="$(ldconfig -p 2>/dev/null | awk '/libjemalloc\.so\.2/ {print $NF; exit}')"
      fi
      [[ -n "$JEMALLOC" ]] || echo "WARNING: jemalloc requested but libjemalloc.so.2 not found; using system allocator (apt install libjemalloc2)" >&2 ;;
    *)
      if [[ -f "$JEMALLOC_REQ" ]]; then JEMALLOC="$JEMALLOC_REQ"
      else echo "WARNING: jemalloc path not found: $JEMALLOC_REQ; using system allocator" >&2; fi ;;
  esac
fi

# ── Run ──────────────────────────────────────────────────────────────────
SUBCMD="${CLI_SUBCMD:-${MOQX_SUBCMD:-serve}}"
USE_SUDO="${CLI_USE_SUDO:-${MOQX_USE_SUDO:-1}}"
CMD=("$MOQX_BIN" "$SUBCMD" --config "$RESOLVED_CONFIG" "${PASSTHRU[@]}")

if (( DRY_RUN )); then
  echo "# relay knobs (templated into config)"
  echo "stack=$MOQX_STACK port=$MOQX_PORT admin_port=$MOQX_ADMIN_PORT endpoint=$MOQX_ENDPOINT insecure=$MOQX_INSECURE"
  echo "threads=$MOQX_THREADS relay_thread=$MOQX_RELAY_THREAD local_fwd=$MOQX_LOCAL_FWD bpf_steering=$MOQX_BPF_STEERING cache=$MOQX_CACHE"
  echo "cc=$MOQX_CC send_pkts=$MOQX_SEND_PKTS recv_pkts=$MOQX_RECV_PKTS udp_buffer=$MOQX_UDP_BUFFER ignore_path_mtu=$MOQX_IGNORE_PATH_MTU"
  echo "# resolved env"
  echo "GLOG_minloglevel=$GLOG_minloglevel GLOG_v=$GLOG_v"
  echo "GLOG_vmodule=$GLOG_vmodule"
  echo "LD_PRELOAD=${JEMALLOC:-<none>}"
  echo "# resolved config: $RESOLVED_CONFIG (from $CONFIG_TEMPLATE)"
  echo "# command"
  if (( USE_SUDO )); then
    printf '  sudo'
    [[ -n "$JEMALLOC" ]] && printf ' %s=%q' LD_PRELOAD "$JEMALLOC"
    printf ' %s=%q' \
      GLOG_v "$GLOG_v" GLOG_vmodule "$GLOG_vmodule" \
      GLOG_minloglevel "$GLOG_minloglevel" \
      GLOG_logtostderr "$GLOG_logtostderr" GLOG_colorlogtostderr "$GLOG_colorlogtostderr"
  else
    [[ -n "$JEMALLOC" ]] && printf '  LD_PRELOAD=%q' "$JEMALLOC"
  fi
  printf ' %q' "${CMD[@]}"; printf '\n'
  exit 0
fi

# ── Pre-flight: warn (don't block) if UDP sysctls are below recommended ───
[[ "$SUBCMD" == serve ]] && { check_sysctl warn || true; }

if (( USE_SUDO )); then
  # Pass GLOG_*/LD_PRELOAD explicitly: sudoers env_reset strips vars not in
  # env_keep, and exporting LD_PRELOAD would be dropped across the boundary.
  SUDO_ENV=(
    GLOG_v="$GLOG_v"
    GLOG_vmodule="$GLOG_vmodule"
    GLOG_minloglevel="$GLOG_minloglevel"
    GLOG_logtostderr="$GLOG_logtostderr"
    GLOG_colorlogtostderr="$GLOG_colorlogtostderr"
  )
  [[ -n "$JEMALLOC" ]] && SUDO_ENV+=("LD_PRELOAD=$JEMALLOC")
  exec sudo "${SUDO_ENV[@]}" "${CMD[@]}"
else
  [[ -n "$JEMALLOC" ]] && export LD_PRELOAD="$JEMALLOC"
  exec "${CMD[@]}"
fi
