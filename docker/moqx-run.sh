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
      --config FILE         config YAML template (default: docker/config.production.yaml)
      --env FILE            alternate .env file (default: docker/.env)
      --bin FILE            moqx binary path (default: <project>/build/moqx)

Execution:
      --no-sudo             run without sudo (cert files must be user-readable)
  -n, --dry-run             print resolved env + command, don't exec
  -h, --help                this help

Environment overrides (via .env or shell):
  MOQX_BIN, MOQX_CONFIG, MOQX_ENV_FILE, MOQX_SUBCMD, MOQX_USE_SUDO
  MOQX_VERBOSE, MOQX_LOG_LEVEL, GLOG_vmodule, DOMAIN

Examples:
  $0                                       # serve with .env + defaults
  $0 -v 4                                  # GLOG_v=4
  $0 -m MoQSession=5,StreamPublisherImpl=5 # targeted trace
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
CONFIG_TEMPLATE="${CLI_CONFIG:-${MOQX_CONFIG:-$SCRIPT_DIR/config.production.yaml}}"

[[ -x "$MOQX_BIN" ]]        || { echo "moqx binary not found: $MOQX_BIN" >&2; exit 1; }
[[ -f "$CONFIG_TEMPLATE" ]] || { echo "config not found: $CONFIG_TEMPLATE" >&2; exit 1; }
command -v envsubst >/dev/null || { echo "envsubst missing (apt install gettext-base)" >&2; exit 1; }

# ── Resolve ${…} placeholders (DOMAIN, ports, etc.) into a temp config ──
RESOLVED_CONFIG=/tmp/moqx-resolved.yaml
envsubst < "$CONFIG_TEMPLATE" > "$RESOLVED_CONFIG"

# ── GLOG — map MOQX_* → GLOG_* (matches docker/entrypoint.sh convention) ─
# Precedence: CLI flag > .env/shell env > fallback default.
export GLOG_logtostderr=1
export GLOG_colorlogtostderr=1
export GLOG_minloglevel="${CLI_LOG_LEVEL:-${MOQX_LOG_LEVEL:-0}}"
export GLOG_v="${CLI_VERBOSE:-${MOQX_VERBOSE:-0}}"
export GLOG_vmodule="${CLI_VMODULE:-${GLOG_vmodule:-MoqxRelay=3,MoQSession=3,MoQForwarder=3,MoQCache=2}}"

# ── Run ──────────────────────────────────────────────────────────────────
SUBCMD="${CLI_SUBCMD:-${MOQX_SUBCMD:-serve}}"
USE_SUDO="${CLI_USE_SUDO:-${MOQX_USE_SUDO:-1}}"

CMD=("$MOQX_BIN" "$SUBCMD" --config "$RESOLVED_CONFIG" "${PASSTHRU[@]}")

if (( DRY_RUN )); then
  echo "# resolved env"
  echo "GLOG_minloglevel=$GLOG_minloglevel"
  echo "GLOG_v=$GLOG_v"
  echo "GLOG_vmodule=$GLOG_vmodule"
  echo "# resolved config: $RESOLVED_CONFIG (from $CONFIG_TEMPLATE)"
  echo "# command"
  if (( USE_SUDO )); then
    printf '  sudo'
    printf ' %s=%q' \
      GLOG_v "$GLOG_v" \
      GLOG_vmodule "$GLOG_vmodule" \
      GLOG_minloglevel "$GLOG_minloglevel" \
      GLOG_logtostderr "$GLOG_logtostderr" \
      GLOG_colorlogtostderr "$GLOG_colorlogtostderr"
  fi
  printf ' %q' "${CMD[@]}"
  printf '\n'
  exit 0
fi

if (( USE_SUDO )); then
  # Pass GLOG_* explicitly: `sudo -E` is unreliable because sudoers
  # env_reset (default on most distros) strips vars not listed in
  # env_keep, and GLOG_* never is. Explicit name=value on the sudo
  # command line bypasses env_reset entirely.
  exec sudo \
    GLOG_v="$GLOG_v" \
    GLOG_vmodule="$GLOG_vmodule" \
    GLOG_minloglevel="$GLOG_minloglevel" \
    GLOG_logtostderr="$GLOG_logtostderr" \
    GLOG_colorlogtostderr="$GLOG_colorlogtostderr" \
    "${CMD[@]}"
else
  exec "${CMD[@]}"
fi
