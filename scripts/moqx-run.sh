#!/usr/bin/env bash
# Local moqx CLI launcher. Honors docker/.env for DOMAIN / MOQX_* / GLOG_*.
# CLI flags win over .env. .env wins over script defaults.
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
                            Bare LEVEL sets root: -x DBG6 = everything
                            CSV for category-specific:
                              -x moxygen=DBG6,quic=INFO
                            Levels: DBG0..DBG9, INFO, WARN, ERR, FATAL
                            (moxygen uses folly XLOG; GLOG_* doesn't apply)

Targeting:
      --subcmd CMD          moqx subcommand (default: serve)
      --config FILE         config YAML template (default: scripts/config.bench.yaml)
      --env FILE            alternate .env file (default: scripts/.env if present)
      --bin FILE            moqx binary path (default: <project>/build/moqx)

Execution:
  -j, --jemalloc [PATH]     LD_PRELOAD jemalloc for the relay (~10% speedup).
                            Bare flag auto-detects libjemalloc.so.2; pass an
                            explicit PATH to override. Warns and continues if
                            not found. (apt install libjemalloc2)
  -b, --bpf-steering        Force mvfst_bpf_steering: true in the config
      --no-bpf-steering     Force mvfst_bpf_steering: false in the config
                            (Linux + mvfst only; default: leave the config's
                            own value untouched)
      --no-sudo             run without sudo (cert files must be user-readable)
  -n, --dry-run             print resolved env + command, don't exec
  -h, --help                this help

Environment overrides (via .env or shell):
  MOQX_BIN, MOQX_CONFIG, MOQX_ENV_FILE, MOQX_SUBCMD, MOQX_USE_SUDO
  MOQX_VERBOSE, MOQX_LOG_LEVEL, GLOG_vmodule, DOMAIN
  MOQX_JEMALLOC       (auto|1|<path> — enable jemalloc preload)
  MOQX_BPF_STEERING   (true|false — override mvfst_bpf_steering)

Examples:
  $0                                       # serve with .env + defaults
  $0 -v 4                                  # GLOG_v=4
  $0 -m MoQSession=5,StreamPublisherImpl=5 # targeted trace
  $0 -j                                    # serve with jemalloc preloaded
  $0 --bpf-steering                        # force CID reuseport steering on
  $0 --subcmd validate-config --no-sudo    # just validate the config
  $0 -n                                    # show what would run
EOF
}

CLI_VERBOSE=""
CLI_LOG_LEVEL=""
CLI_VMODULE=""
CLI_XLOG=""
CLI_SUBCMD=""
CLI_CONFIG=""
CLI_ENV_FILE=""
CLI_BIN=""
CLI_USE_SUDO=""    # "" = unset; "0"/"1" set
CLI_JEMALLOC=""    # "" = unset; "auto" or explicit path
CLI_BPF=""         # "" = unset (respect config); "true"/"false" = override
DRY_RUN=0
PASSTHRU=()

while (($#)); do
  case "$1" in
    -v|--verbose)    CLI_VERBOSE="$2"; shift 2 ;;
    -L|--log-level)  CLI_LOG_LEVEL="$2"; shift 2 ;;
    -m|--vmodule)    CLI_VMODULE="$2"; shift 2 ;;
    -x|--xlog)       CLI_XLOG="$2"; shift 2 ;;
    --subcmd)        CLI_SUBCMD="$2"; shift 2 ;;
    --config)        CLI_CONFIG="$2"; shift 2 ;;
    --env)           CLI_ENV_FILE="$2"; shift 2 ;;
    --bin)           CLI_BIN="$2"; shift 2 ;;
    -j|--jemalloc)
      # Optional arg: a following token that isn't another flag is an
      # explicit .so path; otherwise auto-detect.
      if [[ -n "${2:-}" && "$2" != -* ]]; then
        CLI_JEMALLOC="$2"; shift 2
      else
        CLI_JEMALLOC="auto"; shift
      fi ;;
    -b|--bpf-steering)   CLI_BPF=true;  shift ;;
    --no-bpf-steering)   CLI_BPF=false; shift ;;
    --no-sudo)       CLI_USE_SUDO=0; shift ;;
    -n|--dry-run)    DRY_RUN=1; shift ;;
    -h|--help)       usage; exit 0 ;;
    --)              shift; PASSTHRU+=("$@"); break ;;
    *)               echo "unknown arg: $1" >&2; usage >&2; exit 2 ;;
  esac
done

# ── Folly XLOG passthrough ────────────────────────────────────────────────
# moxygen uses folly::XLOG (not glog VLOG), which ignores GLOG_*. folly
# is initialized via folly::Init in moqx main.cpp and respects the
# --logging=<config> gflag. Bare -x LEVEL sets the ROOT category — all
# XLOGs at that level or higher fire. Pass `cat=LEVEL[,cat=LEVEL...]`
# (a string containing `=`) for category-specific levels.
#   -x DBG6                       → --logging=DBG6           (everything)
#   -x moxygen=DBG6               → --logging=moxygen=DBG6   (one category)
#   -x moxygen=DBG6,quic=INFO     → --logging=moxygen=DBG6,quic=INFO
if [[ -n "$CLI_XLOG" ]]; then
  PASSTHRU+=("--logging=$CLI_XLOG")
fi

# ── Source .env if present ───────────────────────────────────────────────
# set -a exports every KEY=VALUE it parses (shell-compatible .env format).
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

# ── Resolve ${…} placeholders (DOMAIN, ports, etc.) into a temp config ──
RESOLVED_CONFIG=/tmp/moqx-resolved.yaml
envsubst < "$CONFIG_TEMPLATE" > "$RESOLVED_CONFIG"

# ── Optional mvfst_bpf_steering override (CLI > env; unset = respect config) ─
# Patches the resolved temp config only; the template is never modified.
BPF_STEERING="${CLI_BPF:-${MOQX_BPF_STEERING:-}}"
if [[ -n "$BPF_STEERING" ]]; then
  case "$BPF_STEERING" in
    1|true|yes|on)   BPF_STEERING=true ;;
    0|false|no|off)  BPF_STEERING=false ;;
    *) echo "invalid MOQX_BPF_STEERING/--bpf-steering value: $BPF_STEERING (want true/false)" >&2; exit 2 ;;
  esac
  if grep -qE '^[[:space:]]*mvfst_bpf_steering:' "$RESOLVED_CONFIG"; then
    sed -i -E "s/^([[:space:]]*)mvfst_bpf_steering:.*/\1mvfst_bpf_steering: $BPF_STEERING/" "$RESOLVED_CONFIG"
  else
    printf '\nmvfst_bpf_steering: %s\n' "$BPF_STEERING" >> "$RESOLVED_CONFIG"
  fi
fi

# ── GLOG — map MOQX_* → GLOG_* (matches docker/entrypoint.sh convention) ─
# Precedence: CLI flag > .env/shell env > fallback default.
export GLOG_logtostderr=1
export GLOG_colorlogtostderr=1
export GLOG_minloglevel="${CLI_LOG_LEVEL:-${MOQX_LOG_LEVEL:-0}}"
export GLOG_v="${CLI_VERBOSE:-${MOQX_VERBOSE:-0}}"
export GLOG_vmodule="${CLI_VMODULE:-${GLOG_vmodule:-MoqxRelay=3,MoQSession=3,MoQForwarder=3,MoqxCache=2}}"

# ── jemalloc resolution (CLI > env) ──────────────────────────────────────
# moxygen/Meta production uses jemalloc; LD_PRELOAD gives ~10% throughput.
# "auto"/"1"/"true" → search common locations + ldconfig; an explicit path
# is used as-is. Resolves to empty (warn) when not found so the run still
# proceeds with the system allocator.
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
      if [[ -f "$JEMALLOC_REQ" ]]; then
        JEMALLOC="$JEMALLOC_REQ"
      else
        echo "WARNING: jemalloc path not found: $JEMALLOC_REQ; using system allocator" >&2
      fi ;;
  esac
fi

# ── Run ──────────────────────────────────────────────────────────────────
SUBCMD="${CLI_SUBCMD:-${MOQX_SUBCMD:-serve}}"
USE_SUDO="${CLI_USE_SUDO:-${MOQX_USE_SUDO:-1}}"

CMD=("$MOQX_BIN" "$SUBCMD" --config "$RESOLVED_CONFIG" "${PASSTHRU[@]}")

if (( DRY_RUN )); then
  echo "# resolved env"
  echo "GLOG_minloglevel=$GLOG_minloglevel"
  echo "GLOG_v=$GLOG_v"
  echo "GLOG_vmodule=$GLOG_vmodule"
  echo "LD_PRELOAD=${JEMALLOC:-<none>}"
  echo "mvfst_bpf_steering=${BPF_STEERING:-<from config>} ($(grep -E '^[[:space:]]*mvfst_bpf_steering:' "$RESOLVED_CONFIG" | awk '{print $2}' || echo unset) in resolved config)"
  echo "# resolved config: $RESOLVED_CONFIG (from $CONFIG_TEMPLATE)"
  echo "# command"
  if (( USE_SUDO )); then
    printf '  sudo'
    [[ -n "$JEMALLOC" ]] && printf ' %s=%q' LD_PRELOAD "$JEMALLOC"
    printf ' %s=%q' \
      GLOG_v "$GLOG_v" \
      GLOG_vmodule "$GLOG_vmodule" \
      GLOG_minloglevel "$GLOG_minloglevel" \
      GLOG_logtostderr "$GLOG_logtostderr" \
      GLOG_colorlogtostderr "$GLOG_colorlogtostderr"
  else
    [[ -n "$JEMALLOC" ]] && printf '  LD_PRELOAD=%q' "$JEMALLOC"
  fi
  printf ' %q' "${CMD[@]}"
  printf '\n'
  exit 0
fi

if (( USE_SUDO )); then
  # Pass GLOG_*/LD_PRELOAD explicitly: `sudo -E` is unreliable because
  # sudoers env_reset (default on most distros) strips vars not listed in
  # env_keep, and these never are. Explicit name=value on the sudo command
  # line bypasses env_reset entirely. LD_PRELOAD especially: exporting it
  # would be silently dropped across the sudo boundary.
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
