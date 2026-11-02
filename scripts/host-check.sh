#!/usr/bin/env bash
# Report host and relay-process settings that affect relay performance.
# Read-only: changes nothing. See docs/tuning.md.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF="$SCRIPT_DIR/host/99-moqx-quic.conf"
PID="" MODE=report STRICT=0

usage() {
  cat <<EOF
Usage: $(basename "$0") [--pid PID] [--warn] [--strict] [--conf FILE]

  (default)    report the host: CPU, memory, UDP sysctls vs recommended,
               UDP drop counters, open-file and core limits, jemalloc, NIC
  --pid PID    also report that process: limits, jemalloc loaded, MALLOC_CONF,
               threads, and the buffer size and drops of each UDP socket
  --warn       print nothing unless a sysctl is below recommended, then warn
               on stderr and exit 1 (moqx-run.sh runs this before serving)
  --strict     exit 1 if a sysctl is below recommended
  --conf FILE  recommended sysctls (default: host/99-moqx-quic.conf next to
               this script)
EOF
}

while (($#)); do
  case "$1" in
    --pid)     PID="${2:?--pid needs a PID}"; shift 2 ;;
    --warn)    MODE=warn; shift ;;
    --strict)  STRICT=1; shift ;;
    --conf)    CONF="${2:?--conf needs a file}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *)         echo "unknown arg: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -r "$CONF" ]] || { echo "recommended sysctls not found: $CONF" >&2; exit 2; }
[[ -z "$PID" || -d "/proc/$PID" ]] || { echo "no such process: $PID" >&2; exit 2; }

# sysctl via /proc: the sysctl binary is often not on a non-root PATH.
read_sysctl() {
  local f="/proc/sys/${1//.//}"
  [[ -r "$f" ]] && tr -s '[:space:]' ' ' < "$f" | sed 's/ $//'
}

section() { printf '\n== %s\n' "$1"; }
row() { printf '%-24s %s\n' "$1" "$2"; }

# ── Recommended sysctls ───────────────────────────────────────────────────
KEYS=() RECS=() CURS=() STATUS=() LOW=()
while IFS='=' read -r k v; do
  k="${k//[[:space:]]/}"; v="${v//[[:space:]]/}"
  [[ -z "$k" || "$k" == \#* ]] && continue
  cur="$(read_sysctl "$k")"
  if [[ -z "$cur" ]]; then st="n/a"
  elif [[ "$cur" =~ ^[0-9]+$ ]] && (( cur < v )); then st="LOW"; LOW+=("$k=$v")
  else st="ok"; fi
  KEYS+=("$k") RECS+=("$v") CURS+=("${cur:-?}") STATUS+=("$st")
done < "$CONF"

fix_hint() {
  echo "  raise now: sudo sysctl -w ${LOW[*]}"
  echo "  persist:   sudo install -m 0644 $CONF /etc/sysctl.d/ && sudo sysctl -p /etc/sysctl.d/$(basename "$CONF")"
}

if [[ "$MODE" == warn ]]; then
  ((${#LOW[@]})) || exit 0
  { echo "WARNING: UDP sysctls below recommended for a high fan-out relay (see docs/tuning.md):"; fix_hint; } >&2
  exit 1
fi

# ── Host ──────────────────────────────────────────────────────────────────
echo "moqx host check: $(date -u +%Y-%m-%dT%H:%M:%SZ)"

section host
row hostname "$(hostname 2>/dev/null || echo '?')"
row kernel "$(uname -r)"
row os "$(. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-?}" || echo '?')"
row cpu "$(awk -F': ' '/^model name/ {print $2; exit}' /proc/cpuinfo 2>/dev/null)"
row "cpus online" "$(nproc 2>/dev/null || echo '?')"
row memory "$(awk '/^MemTotal:/ {printf "%.1f GiB", $2 / 1048576}' /proc/meminfo)"
gov=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
row "cpu governor" "$([[ -r $gov ]] && cat $gov || echo 'n/a (no cpufreq, usual on VMs)')"
thp=/sys/kernel/mm/transparent_hugepage/enabled
row "transparent hugepages" "$([[ -r $thp ]] && sed -E 's/.*\[([a-z]+)\].*/\1/' $thp || echo n/a)"
if pgrep -x irqbalance >/dev/null 2>&1; then row irqbalance running; else row irqbalance "not running"; fi

# ── Kernel UDP ────────────────────────────────────────────────────────────
section "kernel UDP (recommended values: $CONF)"
printf '%-30s %-14s %-14s %s\n' SYSCTL CURRENT RECOMMENDED STATUS
for i in "${!KEYS[@]}"; do
  printf '%-30s %-14s %-14s %s\n' "${KEYS[$i]}" "${CURS[$i]}" "${RECS[$i]}" "${STATUS[$i]}"
done
if ((${#LOW[@]})); then echo; fix_hint; fi

page="$(getconf PAGESIZE 2>/dev/null || echo 4096)"
read -r m1 m2 m3 <<< "$(read_sysctl net.ipv4.udp_mem)"
if [[ -n "${m3:-}" ]]; then
  echo
  row "net.ipv4.udp_mem" "$m1 $m2 $m3 pages = $(( m1 * page >> 20 )) / $(( m2 * page >> 20 )) / $(( m3 * page >> 20 )) MiB (set at boot from RAM)"
fi
row "udp counters" "$(awk '/^Udp:/ { if (!h) { for (i = 2; i <= NF; i++) n[i] = $i; h = 1 } else { for (i = 2; i <= NF; i++) v[n[i]] = $i } }
  END { printf "InErrors %s  RcvbufErrors %s  SndbufErrors %s  MemErrors %s (since boot)",
        v["InErrors"], v["RcvbufErrors"], v["SndbufErrors"], ("MemErrors" in v ? v["MemErrors"] : "n/a") }' /proc/net/snmp)"
dropped=0 squeezed=0
while read -r _ d s _; do
  dropped=$(( dropped + 16#$d )); squeezed=$(( squeezed + 16#$s ))
done < /proc/net/softnet_stat
row "softnet backlog" "dropped $dropped  time_squeeze $squeezed (since boot)"

# ── Limits ────────────────────────────────────────────────────────────────
section "limits (this shell)"
row "open files" "soft $(ulimit -Sn) / hard $(ulimit -Hn)"
row "core file size" "soft $(ulimit -Sc) / hard $(ulimit -Hc)"
row core_pattern "$(cat /proc/sys/kernel/core_pattern)"

# ── Allocator ─────────────────────────────────────────────────────────────
section allocator
jemalloc=""
for cand in /usr/lib/x86_64-linux-gnu/libjemalloc.so.2 /usr/lib/aarch64-linux-gnu/libjemalloc.so.2 \
            /lib64/libjemalloc.so.2 /usr/lib64/libjemalloc.so.2 /usr/lib/libjemalloc.so.2 \
            /usr/local/lib/libjemalloc.so.2; do
  [[ -f "$cand" ]] && { jemalloc="$cand"; break; }
done
row "jemalloc library" "${jemalloc:-not found (apt install libjemalloc2)}"

# ── NIC ───────────────────────────────────────────────────────────────────
iface="$(awk '$2 == "00000000" {print $1; exit}' /proc/net/route 2>/dev/null)"
section "nic (default route: ${iface:-none})"
if [[ -n "$iface" ]]; then
  row speed "$(cat "/sys/class/net/$iface/speed" 2>/dev/null || echo '?') Mb/s"
  row mtu "$(cat "/sys/class/net/$iface/mtu" 2>/dev/null || echo '?')"
  ethtool="$(PATH="$PATH:/usr/sbin:/sbin" command -v ethtool || true)"
  if [[ -n "$ethtool" ]]; then
    "$ethtool" -k "$iface" 2>/dev/null \
      | grep -E '^(generic-segmentation-offload|tx-udp-segmentation|generic-receive-offload|rx-udp-gro-forwarding):' \
      | while IFS=: read -r name val; do row "$name" "${val# }"; done
  else
    row offloads "ethtool not installed"
  fi
fi

# ── Process ───────────────────────────────────────────────────────────────
if [[ -n "$PID" ]]; then
  section "process $PID ($(cat "/proc/$PID/comm" 2>/dev/null))"
  row threads "$(awk '/^Threads:/ {print $2}' "/proc/$PID/status")"
  row "open files" "$(awk '/^Max open files/ {print "soft " $(NF-2) " / hard " $(NF-1)}' "/proc/$PID/limits")"
  row "core file size" "$(awk '/^Max core file size/ {print "soft " $(NF-2) " / hard " $(NF-1)}' "/proc/$PID/limits")"
  loaded="$(grep -m1 -o '/[^ ]*libjemalloc[^ ]*' "/proc/$PID/maps" 2>/dev/null)"
  case $? in
    0) row "jemalloc loaded" "$loaded" ;;
    1) row "jemalloc loaded" no ;;
    *) row "jemalloc loaded" "unreadable (not this user's process)" ;;
  esac
  if [[ -r "/proc/$PID/environ" ]]; then
    mc="$(tr '\0' '\n' < "/proc/$PID/environ" | sed -n 's/^MALLOC_CONF=//p')"
    row MALLOC_CONF "${mc:-unset}"
  else
    row MALLOC_CONF "unreadable (not this user's process)"
  fi
  if command -v ss >/dev/null; then
    echo "udp sockets (a size the process set shows doubled: Linux adds bookkeeping room):"
    ss -uanpm 2>/dev/null | awk -v pid="$PID" '
      index($0, "users:(") { want = index($0, "pid=" pid ","); if (want) addr = $4 }
      want && index($0, "skmem:(") {
        match($0, /rb[0-9]+/);  rb = substr($0, RSTART + 2, RLENGTH - 2)
        match($0, /tb[0-9]+/);  tb = substr($0, RSTART + 2, RLENGTH - 2)
        match($0, /,d[0-9]+/);  d  = substr($0, RSTART + 2, RLENGTH - 2)
        printf "  %-30s rcvbuf %-10s sndbuf %-10s drops %s\n", addr, rb, tb, d
        want = 0; n++
      }
      END { if (!n) print "  none found (another user'"'"'s sockets need root)" }'
  else
    row "udp sockets" "ss not installed"
  fi
fi

((STRICT && ${#LOW[@]})) && exit 1
exit 0
