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
      --ignore-path-mtu     send full-size pkts, skip PMTU discovery (default: off)
      --bpf-steering        mvfst CID reuseport steering, Linux+mvfst (default: off)

Listener (templated into the config; CLI > .env > default):
      --quic-stack STACK    listener quic stack: mvfst|picoquic (default mvfst)
      --moqt-versions LIST  advertised MoQT drafts in server-preference order,
                            e.g. 16,14,18 (default 16,14,18; first listed wins).
                            Pass a single value (e.g. 18) to pin one draft.
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
      --sudo                run the relay under sudo (only needed to read a
                            root-owned key, e.g. letsencrypt's 0600 privkey;
                            default is no sudo)
  -n, --dry-run             print resolved env + command, don't exec
  -h, --help                this help

Environment overrides (via .env or shell):
  MOQX_BIN, MOQX_CONFIG, MOQX_ENV_FILE, MOQX_SUBCMD, MOQX_USE_SUDO, DOMAIN
  MOQX_VERBOSE, MOQX_LOG_LEVEL, GLOG_vmodule, MOQX_JEMALLOC
  MOQX_THREADS, MOQX_UDP_BUFFER, MOQX_RECV_PKTS, MOQX_SEND_PKTS, MOQX_CC,
  MOQX_LOCAL_FWD, MOQX_CACHE, MOQX_BPF_STEERING, MOQX_IGNORE_PATH_MTU,
  MOQX_RELAY_THREAD, MOQX_STACK, MOQX_MOQT_VERSIONS, MOQX_PORT, MOQX_ADMIN_PORT, MOQX_ENDPOINT,
  MOQX_INSECURE, MOQX_CERT, MOQX_KEY, MOQX_MAX_TRACKS, MOQX_MAX_GROUPS,
  MOQX_RELAY_ID, MOQX_RESOLVED_CONFIG  (env-only; no CLI flag)

Examples:
  $0                                       # serve with .env + defaults
  $0 --threads 8 --udp-buffer 16777216     # 8 IO threads, 16 MB socket buffer
  $0 --recv-pkts 256 -j                    # Alan's recv loop + jemalloc
  $0 --check-sysctl                        # audit kernel UDP buffers, then exit
  $0 --subcmd validate-config              # just validate the config
  DOMAIN=relay.example.com $0 --sudo       # real TLS, root-owned letsencrypt key
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

# normalize a MoQT versions value to a YAML inline list. Pass through an already
# bracketed list (e.g. "[16, 14]"); wrap a comma list ("16,14" -> "[16, 14]").
norm_versions() { local v="${1//[[:space:]]/}"; [[ "$v" == \[*\] ]] && echo "$v" || echo "[${v//,/, }]"; }

CLI_VERBOSE="" CLI_LOG_LEVEL="" CLI_VMODULE="" CLI_XLOG=""
CLI_SUBCMD="" CLI_CONFIG="" CLI_ENV_FILE="" CLI_BIN=""
CLI_USE_SUDO=""   # "" = unset; "0"/"1" set
CLI_JEMALLOC=""   # "" = unset; "auto" or explicit path
CLI_THREADS="" CLI_UDP_BUFFER="" CLI_RECV_PKTS="" CLI_SEND_PKTS="" CLI_CC=""
CLI_LOCAL_FWD="" CLI_CACHE="" CLI_RELAY_THREAD="" CLI_INSECURE=""   # tri-state booleans
CLI_IGNORE_PMTU="" CLI_BPF=""   # tri-state booleans
CLI_STACK="" CLI_MOQT_VERSIONS="" CLI_PORT="" CLI_ADMIN_PORT="" CLI_ENDPOINT="" CLI_CERT="" CLI_KEY=""
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
    --ignore-path-mtu) CLI_IGNORE_PMTU=true; shift ;;
    --bpf-steering)    CLI_BPF=true;         shift ;;
    --quic-stack)    CLI_STACK="$2"; shift 2 ;;
    --moqt-versions) CLI_MOQT_VERSIONS="$2"; shift 2 ;;
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
    --sudo)          CLI_USE_SUDO=1; shift ;;
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
export MOQX_RELAY_ID="${MOQX_RELAY_ID:-moqx-000}"
export MOQX_THREADS="${CLI_THREADS:-${MOQX_THREADS:-4}}"
export MOQX_SEND_PKTS="${CLI_SEND_PKTS:-${MOQX_SEND_PKTS:-16}}"
export MOQX_RECV_PKTS="${CLI_RECV_PKTS:-${MOQX_RECV_PKTS:-256}}"
export MOQX_UDP_BUFFER="${CLI_UDP_BUFFER:-${MOQX_UDP_BUFFER:-$WMEM_MAX}}"
export MOQX_CC="${CLI_CC:-${MOQX_CC:-bbr}}"
MOQX_LOCAL_FWD="$(norm_bool "${CLI_LOCAL_FWD:-${MOQX_LOCAL_FWD:-true}}")"
MOQX_CACHE="$(norm_bool "${CLI_CACHE:-${MOQX_CACHE:-true}}")"
MOQX_RELAY_THREAD="$(norm_bool "${CLI_RELAY_THREAD:-${MOQX_RELAY_THREAD:-true}}")"
# bpf steering and ignore_path_mtu are experimental (Linux+mvfst); default off.
MOQX_BPF_STEERING="$(norm_bool "${CLI_BPF:-${MOQX_BPF_STEERING:-false}}")"
MOQX_IGNORE_PATH_MTU="$(norm_bool "${CLI_IGNORE_PMTU:-${MOQX_IGNORE_PATH_MTU:-false}}")"
for b in MOQX_LOCAL_FWD MOQX_CACHE MOQX_RELAY_THREAD MOQX_BPF_STEERING MOQX_IGNORE_PATH_MTU; do
  [[ "${!b}" == INVALID ]] && { echo "invalid boolean for $b (want true/false)" >&2; exit 2; }
done
export MOQX_LOCAL_FWD MOQX_CACHE MOQX_RELAY_THREAD
export MOQX_BPF_STEERING MOQX_IGNORE_PATH_MTU

# ── Listener knobs (CLI > .env/env > default), exported for envsubst ──────
export MOQX_STACK="${CLI_STACK:-${MOQX_STACK:-mvfst}}"
case "$MOQX_STACK" in mvfst|picoquic) ;; *) echo "invalid --quic-stack: $MOQX_STACK (want mvfst|picoquic)" >&2; exit 2 ;; esac
# MoQT drafts advertised by the listener (ALPN derives from these), in
# server-preference order — the relay picks the first listed version the client
# also supports. Default offers d16, d14, then d18 as a fallback.
export MOQX_MOQT_VERSIONS="$(norm_versions "${CLI_MOQT_VERSIONS:-${MOQX_MOQT_VERSIONS:-16,14,18}}")"
export MOQX_PORT="${CLI_PORT:-${MOQX_PORT:-4433}}"
export MOQX_ADMIN_PORT="${CLI_ADMIN_PORT:-${MOQX_ADMIN_PORT:-8000}}"
export MOQX_ENDPOINT="${CLI_ENDPOINT:-${MOQX_ENDPOINT:-/moq-relay}}"
export MOQX_MAX_TRACKS="${MOQX_MAX_TRACKS:-1000}"
export MOQX_MAX_GROUPS="${MOQX_MAX_GROUPS:-100}"
# Did the user explicitly ask for real TLS (a specific cert/key, or a DOMAIN to
# locate one)? Capture before the defaults below overwrite MOQX_CERT/MOQX_KEY.
CERT_EXPLICIT=0
[[ -n "${CLI_CERT}${CLI_KEY}${MOQX_CERT:-}${MOQX_KEY:-}${DOMAIN:-}" ]] && CERT_EXPLICIT=1

# TLS: cert/key default to letsencrypt under DOMAIN; --insecure ignores them.
export MOQX_CERT="${CLI_CERT:-${MOQX_CERT:-/etc/letsencrypt/live/${DOMAIN:-}/fullchain.pem}}"
export MOQX_KEY="${CLI_KEY:-${MOQX_KEY:-/etc/letsencrypt/live/${DOMAIN:-}/privkey.pem}}"

# Insecure (built-in dev cert) resolution, in priority order:
#   1. An explicit --insecure / MOQX_INSECURE always wins, whatever else is set.
#   2. An explicit cert/key/DOMAIN means "use real TLS" — trust it even when the
#      cert lives under a root-only path (e.g. /etc/letsencrypt) this user can't
#      stat; the relay runs via sudo and reads it. moxygen errors if truly absent.
#   3. Otherwise auto: serve real TLS if the default cert is readable here, else
#      fall back to the dev cert so a bare local bench run needs no TLS setup.
INSECURE_REQ="${CLI_INSECURE:-${MOQX_INSECURE:-}}"
if [[ -n "$INSECURE_REQ" ]]; then
  MOQX_INSECURE="$(norm_bool "$INSECURE_REQ")"
  [[ "$MOQX_INSECURE" == INVALID ]] && { echo "invalid boolean for MOQX_INSECURE (want true/false)" >&2; exit 2; }
elif (( CERT_EXPLICIT )); then
  MOQX_INSECURE=false
elif [[ -f "$MOQX_CERT" ]]; then
  MOQX_INSECURE=false
else
  MOQX_INSECURE=true
  echo "note: TLS cert not found ($MOQX_CERT); using built-in dev cert. Set DOMAIN or --cert/--key for real TLS, or pass --insecure to silence." >&2
fi
export MOQX_INSECURE

# ── Resolve placeholders into a temp config ───────────────────────────────
# Fixed path by default; override (e.g. per perf run, to avoid concurrent
# clobber) with MOQX_RESOLVED_CONFIG.
RESOLVED_CONFIG="${MOQX_RESOLVED_CONFIG:-/tmp/moqx-resolved.yaml}"
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
        /usr/lib/libjemalloc.so.2 \
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
USE_SUDO="${CLI_USE_SUDO:-${MOQX_USE_SUDO:-0}}"
CMD=("$MOQX_BIN" "$SUBCMD" --config "$RESOLVED_CONFIG" "${PASSTHRU[@]}")

if (( DRY_RUN )); then
  echo "# relay knobs (templated into config)"
  echo "relay_id=$MOQX_RELAY_ID stack=$MOQX_STACK moqt_versions=$MOQX_MOQT_VERSIONS port=$MOQX_PORT admin_port=$MOQX_ADMIN_PORT endpoint=$MOQX_ENDPOINT insecure=$MOQX_INSECURE"
  echo "resolved_config=$RESOLVED_CONFIG"
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

# ── Pre-flight: real-TLS cert/key must be readable by whoever runs the relay ─
# moxygen SIGABRTs ("no certificates read") on a missing/unreadable cert (#173),
# so probe with the same privilege the relay will use and fail cleanly instead.
if [[ "$SUBCMD" == serve && "$MOQX_INSECURE" == false ]]; then
  if (( USE_SUDO )); then probe=(sudo test -r); else probe=(test -r); fi
  for f in "$MOQX_CERT" "$MOQX_KEY"; do
    "${probe[@]}" "$f" 2>/dev/null || {
      echo "error: TLS cert/key not readable: $f" >&2
      if [[ -e "$f" ]]; then
        echo "  - file exists but isn't readable as $(id -un); if it's root-owned (e.g." >&2
        echo "    letsencrypt's 0600 privkey), re-run with --sudo" >&2
      fi
      echo "  - check DOMAIN spelling — it names the /etc/letsencrypt/live/<dir>, not the served host" >&2
      echo "    (a wildcard *.example.com cert lives under the 'example.com' dir)" >&2
      echo "  - or pass --cert/--key explicitly, or --insecure for the built-in dev cert" >&2
      exit 1
    }
  done
fi

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
