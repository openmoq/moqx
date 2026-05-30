#!/usr/bin/env bash
# perf-metrics.sh — generic moqx /metrics poller
#
# Discovers all metric names on startup, prints a tab-separated header row,
# then one row per second of per-second deltas (counters), interval averages
# (histograms), and instantaneous values (gauges), plus system stats.
#
# Bucket lines are skipped; histogram _sum/_count pairs are collapsed into a
# single avg_ column.
#
# Usage: scripts/perf-metrics.sh [admin_port] [log_file]
#   admin_port  default 19701
#   log_file    default /tmp/moqx_metrics_<timestamp>.log
#
# Terminal tip: column -t -s $'\t'   or   awk -F'\t' '{print $N}' to slice

set -uo pipefail

ADMIN_PORT="${1:-19701}"
LOG_FILE="${2:-/tmp/moqx_metrics_$(date +%Y%m%d_%H%M%S).log}"

SEP=$'\t'
CLK_TCK=$(getconf CLK_TCK 2>/dev/null || echo 100)

# ---------------------------------------------------------------------------
# parse_prom: prometheus text → "KEY VALUE" lines, one per metric.
#   - strips moqx_ prefix
#   - condenses labels to values only: {role="io",le="1000"} → [io,1000]
#   - drops _bucket lines and comment/empty lines
# ---------------------------------------------------------------------------
parse_prom() {
  awk '
    /^#/ || /^$/ { next }
    {
      if (match($0, /\{[^}]*\}/)) {
        name = substr($0, 1, RSTART-1)
        labels = substr($0, RSTART+1, RLENGTH-2)
        rest = substr($0, RSTART+RLENGTH)
        sub(/^ +/, "", rest); sub(/ .*/, "", rest); val = rest
        gsub(/[a-zA-Z_]+=/, "", labels)
        gsub(/"/, "", labels)
        key = name "[" labels "]"
      } else {
        key = $1; val = $2
      }
      if (key ~ /_bucket/) next
      sub(/^moqx_/, "", key)
      print key " " val
    }
  '
}

# ---------------------------------------------------------------------------
# col_name: human-readable column header for a raw key
#   quicBytesWritten_total        → quicBytesWritten/s
#   quicRttSample_milliseconds_sum → avg_quicRttSample_ms
#   evbLoopBusy_microseconds_sum[io] → avg_evbLoopBusy_us[io]
#   quicActiveConnections         → quicActiveConnections  (gauge, unchanged)
# ---------------------------------------------------------------------------
col_name() {
  local key="$1"
  if [[ "$key" == *_total || "$key" == *_total\[* ]]; then
    echo "${key/_total//s}"
  elif [[ "$key" == *_sum || "$key" == *_sum\[* ]]; then
    local base="${key/_sum/}"
    base="${base/_milliseconds/_ms}"
    base="${base/_microseconds/_us}"
    base="${base/_bytes/_B}"
    base="${base/_bits_per_second/_bps}"
    base="${base/_packets/_pkts}"
    echo "avg_${base}"
  else
    echo "$key"
  fi
}

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
declare -A prev=()
declare -A sys_prev=()
declare -A cur=()
declare -a col_order=()
header_printed=false
start_ms=$(date +%s%3N)
prev_tick_ms=$start_ms
prev_relay_cpu=0
declare -A io_prev_cpu=()   # keyed by thread name (e.g. moqx-io0)
declare -A io_tid=()        # thread name → tid
declare -a io_names=()      # ordered list of discovered io thread names

# Sleep only the remainder of the current 1s tick window.
tick_sleep() {
  local rem=$(( tick_start_ms + 1000 - $(date +%s%3N) ))
  [[ $rem -gt 0 ]] && sleep "$(awk -v ms="$rem" 'BEGIN { printf "%.3f", ms/1000 }')"
}

while true; do
  tick_start_ms=$(date +%s%3N)
  raw=$(curl -sf "http://localhost:${ADMIN_PORT}/metrics" 2>/dev/null) || { tick_sleep; continue; }

  cur=()
  while read -r key val; do
    cur["$key"]="$val"
  done < <(printf '%s\n' "$raw" | parse_prom)

  now_ms=$(date +%s%3N)
  elapsed=$(( (now_ms - start_ms) / 1000 ))
  interval_s=$(awk -v d="$(( now_ms - prev_tick_ms ))" 'BEGIN { printf "%.3f", d/1000 }')

  # System stats
  relay_pid=$(pgrep -x moqx 2>/dev/null | head -1 || true)
  cpu_pct="?" rss_mb="?" cur_relay_cpu=0
  declare -A io_cpu=()
  if [[ -n "$relay_pid" ]] && [[ -r "/proc/$relay_pid/stat" ]]; then
    stat_info=$(awk '{print $14+$15, $24}' "/proc/$relay_pid/stat" 2>/dev/null || true)
    if [[ -n "$stat_info" ]]; then
      cur_relay_cpu=${stat_info%% *}
      rss_pages=${stat_info##* }
      rss_mb=$(awk -v p="$rss_pages" 'BEGIN { printf "%.0f", p * 4 / 1024 }')
      cpu_pct=$(awk -v d="$(( cur_relay_cpu - prev_relay_cpu ))" \
                    -v hz="$CLK_TCK" \
                    -v ms="$(( now_ms - prev_tick_ms ))" \
                'BEGIN { if (ms > 0) printf "%.1f", d/hz*1000/ms*100; else printf "0.0" }')
    fi

    # Discover io threads on first sighting (names like moqx-io*)
    if [[ ${#io_names[@]} -eq 0 ]]; then
      for tid_dir in /proc/$relay_pid/task/*/; do
        local_tid="${tid_dir%/}"; local_tid="${local_tid##*/}"
        tname=$(cat "/proc/$relay_pid/task/$local_tid/comm" 2>/dev/null || true)
        if [[ "$tname" == moqx-io* ]]; then
          io_tid["$tname"]="$local_tid"
          io_names+=("$tname")
        fi
      done
      mapfile -t io_names < <(printf '%s\n' "${io_names[@]}" | sort)
    fi

    # Per-io-thread CPU%
    for tname in "${io_names[@]}"; do
      local_tid="${io_tid[$tname]}"
      cur_ticks=$(awk '{print $14+$15}' "/proc/$relay_pid/task/$local_tid/stat" 2>/dev/null || echo 0)
      prev_ticks="${io_prev_cpu[$tname]:-0}"
      io_cpu[$tname]=$(awk -v d="$(( cur_ticks - prev_ticks ))" \
                           -v hz="$CLK_TCK" \
                           -v ms="$(( now_ms - prev_tick_ms ))" \
                       'BEGIN { if (ms>0) printf "%.1f", d/hz*1000/ms*100; else printf "0.0" }')
      io_prev_cpu["$tname"]="$cur_ticks"
    done
  fi

  lo_now=$(awk '/^ *lo:/  { print $10+0 }' /proc/net/dev)
  tx_now=$(awk 'NR>2 && !/^ *lo:/ { sum += $10 } END { print sum+0 }' /proc/net/dev)
  udp_snmp=$(awk '/^Udp:/ { if (!h) { h=$0 } else { d=$0; exit } } END { print h"\n"d }' \
             /proc/net/snmp)
  udp_err=$(paste <(printf '%s\n' "$udp_snmp" | head -1 | tr ' ' '\n') \
                  <(printf '%s\n' "$udp_snmp" | tail -1 | tr ' ' '\n') \
            | awk '$1=="InErrors" { print $2 }')
  udp_buf=$(paste <(printf '%s\n' "$udp_snmp" | head -1 | tr ' ' '\n') \
                  <(printf '%s\n' "$udp_snmp" | tail -1 | tr ' ' '\n') \
            | awk '$1=="RcvbufErrors" { print $2 }')

  # First tick: discover column order and print header
  if [[ "$header_printed" == false ]]; then
    for key in "${!cur[@]}"; do
      [[ "$key" == *_count || "$key" == *_count\[* ]] && continue
      col_order+=("$key")
    done
    mapfile -t col_order < <(printf '%s\n' "${col_order[@]}" | sort)

    header="elapsed"
    for key in "${col_order[@]}"; do
      header+="${SEP}$(col_name "$key")"
    done
    header+="${SEP}CPU%"
    for tname in "${io_names[@]}"; do header+="${SEP}${tname}_CPU%"; done
    header+="${SEP}RSS_MB${SEP}lo_Mbps${SEP}ext_Mbps${SEP}UDPErr/s${SEP}UDPBufDrop/s"
    printf '%s\n' "$header" | tee "$LOG_FILE"

    for key in "${!cur[@]}"; do prev["$key"]="${cur[$key]}"; done
    sys_prev[lo]="${lo_now:-0}"
    sys_prev[tx]="${tx_now:-0}"
    sys_prev[udp_err]="${udp_err:-0}"
    sys_prev[udp_buf]="${udp_buf:-0}"
    prev_relay_cpu=$cur_relay_cpu
    prev_tick_ms=$now_ms
    header_printed=true
    tick_sleep
    continue
  fi

  # Emit data row
  row="$elapsed"
  for key in "${col_order[@]}"; do
    cur_val="${cur[$key]:-0}"
    prev_val="${prev[$key]:-0}"
    if [[ "$key" == *_total || "$key" == *_total\[* ]]; then
      val=$(awk -v c="$cur_val" -v p="$prev_val" -v s="$interval_s" \
            'BEGIN { if (s>0) printf "%.1f", (c-p)/s; else printf "0.0" }')
    elif [[ "$key" == *_sum || "$key" == *_sum\[* ]]; then
      count_key="${key/_sum/_count}"
      cur_cnt="${cur[$count_key]:-0}"
      prev_cnt="${prev[$count_key]:-0}"
      val=$(awk -v cs="$cur_val" -v ps="$prev_val" \
                -v cc="$cur_cnt"  -v pc="$prev_cnt" \
            'BEGIN { s=cs-ps; c=cc-pc; if (c>0) printf "%.2f", s/c; else printf "0.00" }')
    else
      val="$cur_val"
    fi
    row+="${SEP}${val}"
  done

  row+="${SEP}${cpu_pct}"
  for tname in "${io_names[@]}"; do row+="${SEP}${io_cpu[$tname]:-?}"; done
  dlo=$(( ${lo_now:-0}        - ${sys_prev[lo]:-0} ))
  dtx=$(( ${tx_now:-0}        - ${sys_prev[tx]:-0} ))
  dudp_err=$(( ${udp_err:-0}  - ${sys_prev[udp_err]:-0} ))
  dudp_buf=$(( ${udp_buf:-0}  - ${sys_prev[udp_buf]:-0} ))
  lo_mbps=$(awk  -v d="$dlo" -v s="$interval_s" 'BEGIN { if (s>0) printf "%.2f", d*8/1e6/s; else printf "0.00" }')
  ext_mbps=$(awk -v d="$dtx" -v s="$interval_s" 'BEGIN { if (s>0) printf "%.2f", d*8/1e6/s; else printf "0.00" }')
  derr_s=$(awk   -v d="$dudp_err" -v s="$interval_s" 'BEGIN { if (s>0) printf "%.1f", d/s; else printf "0.0" }')
  dbuf_s=$(awk   -v d="$dudp_buf" -v s="$interval_s" 'BEGIN { if (s>0) printf "%.1f", d/s; else printf "0.0" }')

  row+="${SEP}${rss_mb}${SEP}${lo_mbps}${SEP}${ext_mbps}${SEP}${derr_s}${SEP}${dbuf_s}"
  printf '%s\n' "$row" | tee -a "$LOG_FILE"

  for key in "${!cur[@]}"; do prev["$key"]="${cur[$key]}"; done
  sys_prev[lo]="${lo_now:-0}"
  sys_prev[tx]="${tx_now:-0}"
  sys_prev[udp_err]="${udp_err:-0}"
  sys_prev[udp_buf]="${udp_buf:-0}"
  prev_relay_cpu=$cur_relay_cpu
  prev_tick_ms=$now_ms
  tick_sleep
done
