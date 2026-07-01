#!/usr/bin/env bash
# log-cleanup.sh — Manual retention cleanup for moqx log files (.mlog, .qlog, etc.).
#
# Usage:
#   log-cleanup.sh --dir <path> [--ext <.EXT>] [--max-age-days <N>] [--max-dir-mb <N>] [--dry-run]
#
# Options:
#   --dir <path>          Directory containing log files (required).
#   --ext <.EXT>          File extension to match (default: .mlog). Use .qlog for qlog cleanup.
#   --max-age-days <N>    Delete files last modified more than N days ago.
#                         N must be >= 1.
#   --max-dir-mb <N>      After age-based deletion, trim the directory to N MB
#                         by deleting the oldest files first.
#                         N must be >= 1.
#   --dry-run             Print what would be deleted without actually deleting.
#   -h, --help            Show this help message.
#
# At least one of --max-age-days or --max-dir-mb must be provided.
#
# Examples:
#   log-cleanup.sh --dir /var/log/moqx/mlog --max-age-days 7 --max-dir-mb 1024
#   log-cleanup.sh --dir /var/log/moqx/qlog --ext .qlog --max-age-days 3 --max-dir-mb 2048

set -euo pipefail

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

DIR=""
EXT=".mlog"
MAX_AGE_DAYS=""
MAX_DIR_MB=""
DRY_RUN=0

usage() {
  grep '^#' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dir)
      DIR="${2:?'--dir requires a path argument'}"
      shift 2
      ;;
    --ext)
      EXT="${2:?'--ext requires an extension argument (e.g. .mlog or .qlog)'}"
      shift 2
      ;;
    --max-age-days)
      MAX_AGE_DAYS="${2:?'--max-age-days requires a numeric argument'}"
      shift 2
      ;;
    --max-dir-mb)
      MAX_DIR_MB="${2:?'--max-dir-mb requires a numeric argument'}"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage 1
      ;;
  esac
done

# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

if [[ -z "$DIR" ]]; then
  echo "Error: --dir is required." >&2
  usage 1
fi

if [[ -z "$MAX_AGE_DAYS" && -z "$MAX_DIR_MB" ]]; then
  echo "Error: at least one of --max-age-days or --max-dir-mb must be provided." >&2
  usage 1
fi

if [[ -n "$MAX_AGE_DAYS" ]]; then
  if ! [[ "$MAX_AGE_DAYS" =~ ^[1-9][0-9]*$ ]]; then
    echo "Error: --max-age-days must be an integer >= 1, got '$MAX_AGE_DAYS'." >&2
    exit 1
  fi
fi

if [[ -n "$MAX_DIR_MB" ]]; then
  if ! [[ "$MAX_DIR_MB" =~ ^[1-9][0-9]*$ ]]; then
    echo "Error: --max-dir-mb must be an integer >= 1, got '$MAX_DIR_MB'." >&2
    exit 1
  fi
fi

if [[ ! -d "$DIR" ]]; then
  echo "Error: directory not found or not accessible: $DIR" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

log() { echo "[log-cleanup] $*"; }
warn() { echo "[log-cleanup] WARNING: $*" >&2; }

remove_file() {
  local path="$1"
  if [[ "$DRY_RUN" -eq 1 ]]; then
    log "dry-run: would delete $path"
  else
    rm -f -- "$path"
    log "deleted $path"
  fi
}

# ---------------------------------------------------------------------------
# Phase 1: Age-based deletion
# ---------------------------------------------------------------------------

if [[ -n "$MAX_AGE_DAYS" ]]; then
  log "Phase 1: removing *${EXT} files older than ${MAX_AGE_DAYS} day(s) in '$DIR'..."
  while IFS= read -r -d '' file; do
    remove_file "$file"
  done < <(find "$DIR" -maxdepth 1 -name "*${EXT}" -type f \
    -mtime +"$((MAX_AGE_DAYS - 1))" -print0)
fi

# ---------------------------------------------------------------------------
# Phase 2: Size-based deletion (oldest-first)
# ---------------------------------------------------------------------------

if [[ -n "$MAX_DIR_MB" ]]; then
  MAX_DIR_BYTES=$(( MAX_DIR_MB * 1024 * 1024 ))

  # Build a list of (mtime_epoch path size_bytes) sorted oldest-first.
  # stat flags differ between macOS/BSD and Linux.
  if stat --version &>/dev/null 2>&1; then
    # GNU stat (Linux)
    stat_fmt=( stat --printf='%Y %n %s\n' )
  else
    # BSD/macOS stat
    stat_fmt=( stat -f '%m %N %z' )
  fi

  # Collect current .mlog files with mtime and size.
  mapfile -d '' files < <(find "$DIR" -maxdepth 1 -name "*${EXT}" -type f -print0)

  if [[ ${#files[@]} -eq 0 ]]; then
    log "Phase 2: no *${EXT} files found, skipping size check."
  else
    # Build sortable lines: "<epoch> <size> <path>"
    tmp_list=()
    total_bytes=0
    for f in "${files[@]}"; do
      read -r mtime_epoch fpath fsize < <("${stat_fmt[@]}" -- "$f" 2>/dev/null \
        | awk '{print $1, $2, $3}') || { warn "could not stat $f, skipping"; continue; }
      tmp_list+=( "$mtime_epoch $fsize $fpath" )
      total_bytes=$(( total_bytes + fsize ))
    done

    log "Phase 2: current directory size is $(( total_bytes / 1024 / 1024 )) MB," \
        "limit is ${MAX_DIR_MB} MB."

    if (( total_bytes > MAX_DIR_BYTES )); then
      # Sort oldest-first (ascending mtime).
      while IFS= read -r line; do
        if (( total_bytes <= MAX_DIR_BYTES )); then
          break
        fi
        fpath="${line#* * }"        # strip "<epoch> <size> "
        fsize="${line%% *}"         # first field is epoch; second is size
        # Re-parse properly:
        read -r _epoch _size _path <<< "$line"
        remove_file "$_path"
        if [[ "$DRY_RUN" -eq 0 ]]; then
          total_bytes=$(( total_bytes - _size ))
        fi
      done < <(printf '%s\n' "${tmp_list[@]}" | sort -n)
    else
      log "Phase 2: directory is within the size limit, nothing to delete."
    fi
  fi
fi

log "Done."
