#!/bin/bash
# run_topn_comprehensive_perf.sh — Run comprehensive perf test with full + steady-state measurements.
#
# Produces:
#   - Full-duration flamegraph + analysis (includes startup/connection phase)
#   - Steady-state-only flamegraph + analysis (excludes first 15s warmup)
#   - Speech statistics
#   - CPU breakdown comparison
#   - Measurement methodology documentation
#
# Usage:
#   ./scripts/run_topn_comprehensive_perf.sh -p 90 -s 1200 --mixed "1,5,10,25,45,65,90" --panelist-n 25 --hz 10

set -e

# ============================================================================
# Configuration
# ============================================================================

PANELISTS=90
SUBSCRIBERS=1200
TOP_N=25
MIXED_TOPN=""
PANELIST_TOPN=""
DURATION=90
UPDATE_HZ=10
SERVER="admin@snk-dev-1.m10x.org"
SSH_KEY="$HOME/.ssh/keys/snk-dev-server.pem"
SSH="ssh -i $SSH_KEY -o IdentitiesOnly=yes -o ConnectTimeout=10 -o ServerAliveInterval=30"
SCP="scp -i $SSH_KEY -o IdentitiesOnly=yes"
PERF_HZ=4999
WARMUP_SECS=20
OPEN=true

usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  -p NUM        Number of panelists/pub-subscribers (default: 90)"
    echo "  -s NUM        Number of pure subscribers (default: 1200)"
    echo "  -n NUM        Top-N value (default: 25)"
    echo "  --mixed STR   Comma-separated N values for pure subs"
    echo "  --panelist-n  Top-N for panelists (default: same as --top_n)"
    echo "  --hz NUM      Update frequency in Hz (default: 10)"
    echo "  -d NUM        Test duration in seconds (default: 90)"
    echo "  --warmup NUM  Warmup seconds to exclude for steady-state (default: 20)"
    echo "  --no-open     Don't open report in browser"
    echo "  -h|--help     Show this help"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case $1 in
        -p) PANELISTS=$2; shift 2;;
        -s) SUBSCRIBERS=$2; shift 2;;
        -n) TOP_N=$2; shift 2;;
        --mixed) MIXED_TOPN=$2; shift 2;;
        --panelist-n) PANELIST_TOPN=$2; shift 2;;
        --hz) UPDATE_HZ=$2; shift 2;;
        -d) DURATION=$2; shift 2;;
        --warmup) WARMUP_SECS=$2; shift 2;;
        --no-open) OPEN=false; shift;;
        -h|--help) usage;;
        *) echo "Unknown option: $1"; usage;;
    esac
done

TOTAL_CONN=$((PANELISTS + SUBSCRIBERS))

# ============================================================================
# Setup output directory
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TIMESTAMP=$(date +%Y%m%d-%H%M%S)

if [ -n "$MIXED_TOPN" ]; then
    TEST_NAME="comprehensive_${PANELISTS}p_${SUBSCRIBERS}s_${UPDATE_HZ}hz"
else
    TEST_NAME="comprehensive_n${TOP_N}_${PANELISTS}p_${SUBSCRIBERS}s_${UPDATE_HZ}hz"
fi

OUTPUT_DIR="${SCRIPT_DIR}/perf_reports/${TIMESTAMP}_${TEST_NAME}"
mkdir -p "$OUTPUT_DIR"

echo "================================================================================"
echo "  moqx Top-N Comprehensive Performance Analysis"
echo "================================================================================"
echo ""
echo "  Configuration:"
echo "    Panelists:        $PANELISTS (pub+sub, N=${PANELIST_TOPN:-$TOP_N})"
echo "    Pure Subscribers: $SUBSCRIBERS (mixed N=[${MIXED_TOPN:-$TOP_N}])"
echo "    Total Connections:$TOTAL_CONN"
echo "    Update Hz:        $UPDATE_HZ"
echo "    Duration:         ${DURATION}s (warmup: ${WARMUP_SECS}s)"
echo "    Perf Sampling:    ${PERF_HZ} Hz"
echo "    Server:           $SERVER"
echo ""
echo "  Output:             $OUTPUT_DIR"
echo ""

# ============================================================================
# Step 1: Collect system information
# ============================================================================

echo "[1/8] Collecting system information..."

SYSINFO=$($SSH $SERVER 'echo "hostname: $(hostname)"; echo "kernel: $(uname -r)"; echo "arch: $(uname -m)"; echo "cpus: $(nproc)"; echo "cpu_model: $(grep -m1 "model name\|Hardware" /proc/cpuinfo | cut -d: -f2 | xargs)"; echo "memory_total: $(free -h | awk "/Mem:/{print \$2}")"; echo "memory_avail: $(free -h | awk "/Mem:/{print \$7}")"; echo "ulimit_nofile: $(ulimit -n)"; echo "perf_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid)"')

echo "$SYSINFO" > "$OUTPUT_DIR/sysinfo.txt"
echo "  $(echo "$SYSINFO" | grep hostname)"
echo "  $(echo "$SYSINFO" | grep cpus)"
echo "  $(echo "$SYSINFO" | grep memory_total)"
echo ""

# ============================================================================
# Step 2: Start relay
# ============================================================================

echo "[2/8] Starting relay..."

$SSH $SERVER "pkill moqx || true" 2>/dev/null
sleep 1
$SSH -f $SERVER "ulimit -n 65536; nohup ~/moqx/build/moqx --config ~/moqx/config.example.yaml > /tmp/relay_perf.log 2>&1 &"
sleep 2

if ! $SSH $SERVER "pgrep moqx > /dev/null 2>&1"; then
    echo "ERROR: Relay failed to start."
    exit 1
fi

RELAY_PID=$($SSH $SERVER "pgrep moqx")
echo "  Relay PID: $RELAY_PID"
echo ""

# ============================================================================
# Step 3: Run perf record (FULL duration) + load test
# ============================================================================

echo "[3/8] Running FULL perf record + load test (${DURATION}s at ${PERF_HZ}Hz sampling)..."

LOAD_TEST_ARGS="--relay_url 'https://localhost:9668/moq-relay' --insecure true"
LOAD_TEST_ARGS="$LOAD_TEST_ARGS --panelists $PANELISTS --subscribers $SUBSCRIBERS"
LOAD_TEST_ARGS="$LOAD_TEST_ARGS --speech_mode true --update_hz $UPDATE_HZ --duration $DURATION"

if [ -n "$MIXED_TOPN" ]; then
    LOAD_TEST_ARGS="$LOAD_TEST_ARGS --mixed_topn '$MIXED_TOPN'"
else
    LOAD_TEST_ARGS="$LOAD_TEST_ARGS --top_n $TOP_N"
fi

if [ -n "$PANELIST_TOPN" ]; then
    LOAD_TEST_ARGS="$LOAD_TEST_ARGS --panelist_topn $PANELIST_TOPN"
fi

PERF_WINDOW=$((DURATION + 15))

$SSH $SERVER "ulimit -n 65536; sudo perf record -F $PERF_HZ -p $RELAY_PID -g -o /tmp/perf_full.data -- sleep $PERF_WINDOW &
PERF_PID=\$!
sleep 2
~/moqx/build/test/track_filter_load_test $LOAD_TEST_ARGS 2>&1
wait \$PERF_PID 2>/dev/null || true" > "$OUTPUT_DIR/test_output.txt"

# Extract key metrics
echo ""
echo "  Test Results:"
grep -E "(Objects Published|Objects Received|Self-Received|Forward Errors|Message Rate|Overall Status|Test Duration|Speech)" "$OUTPUT_DIR/test_output.txt" | sed 's/^  /    /' || true
echo ""

# ============================================================================
# Step 4: Generate FULL flamegraph + reports
# ============================================================================

echo "[4/8] Generating FULL flamegraph (entire test duration)..."

$SSH $SERVER "sudo perf script -i /tmp/perf_full.data 2>/dev/null | ~/FlameGraph/stackcollapse-perf.pl > /tmp/collapsed_full.txt 2>/dev/null"

TITLE_FULL="moqx relay — ${TEST_NAME} — FULL (${DURATION}s incl. warmup)"
$SSH $SERVER "cat /tmp/collapsed_full.txt | ~/FlameGraph/flamegraph.pl --title '$TITLE_FULL' --width 1400 > /tmp/flamegraph_full.svg 2>/dev/null"

$SSH $SERVER 'sudo perf report -i /tmp/perf_full.data --stdio --no-children --sort=symbol --percent-limit=0.3 2>/dev/null | grep -E "^\s+[0-9]+\.[0-9]+%" | sed "s/^\s\+//; s/\s\+\[.\]\s\+/  /" | sed "s/\s\+-\s\+-\s*$//"' > "$OUTPUT_DIR/perf_full_self.txt"
$SSH $SERVER 'sudo perf report -i /tmp/perf_full.data --stdio --children --sort=symbol --percent-limit=0.3 2>/dev/null | grep -E "^\s+[0-9]+\.[0-9]+%\s+[0-9]" | sed "s/^\s\+//; s/\s\+\[.\]\s\+/  /" | sed "s/\s\+-\s\+-\s*$//"' > "$OUTPUT_DIR/perf_full_inclusive.txt"

$SCP $SERVER:/tmp/collapsed_full.txt "$OUTPUT_DIR/collapsed_full.txt" 2>/dev/null
$SCP $SERVER:/tmp/flamegraph_full.svg "$OUTPUT_DIR/flamegraph_full.svg" 2>/dev/null

echo "  Done: $OUTPUT_DIR/flamegraph_full.svg"
echo ""

# ============================================================================
# Step 5: Generate STEADY-STATE flamegraph (exclude warmup)
# ============================================================================

echo "[5/8] Generating STEADY-STATE flamegraph (excluding first ${WARMUP_SECS}s)..."

# Use perf script --time to filter to steady-state window
STEADY_START="${WARMUP_SECS}.0"
STEADY_END="${PERF_WINDOW}.0"

# Filter collapsed stacks by time offset — use perf script time filtering
$SSH $SERVER "sudo perf script -i /tmp/perf_full.data --time ${STEADY_START},${STEADY_END} 2>/dev/null | ~/FlameGraph/stackcollapse-perf.pl > /tmp/collapsed_steady.txt 2>/dev/null"

TITLE_STEADY="moqx relay — ${TEST_NAME} — STEADY STATE (${WARMUP_SECS}s-${DURATION}s)"
$SSH $SERVER "cat /tmp/collapsed_steady.txt | ~/FlameGraph/flamegraph.pl --title '$TITLE_STEADY' --width 1400 > /tmp/flamegraph_steady.svg 2>/dev/null"

# Generate steady-state-only reports via separate perf report is not easily filterable,
# so we use the collapsed stacks to compute percentages
$SSH $SERVER "awk '{print \$NF, \$0}' /tmp/collapsed_steady.txt | sort -rn | awk '{
  total += \$1
}
END {
  # Reprocess
}' /tmp/collapsed_steady.txt" 2>/dev/null || true

# Self-time from collapsed stacks (last frame)
$SSH $SERVER 'total=$(awk "{sum+=\$NF} END {print sum}" /tmp/collapsed_steady.txt); awk -F";" "{split(\$NF, a, \" \"); func=a[1]; for(i=2;i<length(a);i++) func=func\" \"a[i]; sub(/ [0-9]+$/, \"\", func); counts[func]+=\$NF} END {for(f in counts) printf \"%.2f%%  %s\n\", 100.0*counts[f]/'"$total"', f}" /tmp/collapsed_steady.txt | sort -rn | head -50' > "$OUTPUT_DIR/perf_steady_self.txt" 2>/dev/null || true

# If that approach fails (awk complexity), fall back to perf report with time filter
if [ ! -s "$OUTPUT_DIR/perf_steady_self.txt" ]; then
    # Fallback: generate steady-state self-time from collapsed stacks differently
    $SSH $SERVER "python3 -c \"
import sys
from collections import defaultdict
counts = defaultdict(int)
total = 0
with open('/tmp/collapsed_steady.txt') as f:
    for line in f:
        parts = line.rsplit(' ', 1)
        if len(parts) == 2:
            stack, count = parts[0], int(parts[1])
            total += count
            frames = stack.split(';')
            if frames:
                counts[frames[-1]] += count
for func, c in sorted(counts.items(), key=lambda x: -x[1])[:50]:
    print(f'{100.0*c/total:.2f}%  {func}')
\"" > "$OUTPUT_DIR/perf_steady_self.txt" 2>/dev/null
fi

# Inclusive time from collapsed stacks (any frame in stack)
$SSH $SERVER "python3 -c \"
import sys
from collections import defaultdict
counts = defaultdict(int)
total = 0
with open('/tmp/collapsed_steady.txt') as f:
    for line in f:
        parts = line.rsplit(' ', 1)
        if len(parts) == 2:
            stack, count = parts[0], int(parts[1])
            total += count
            seen = set()
            for frame in stack.split(';'):
                if frame not in seen:
                    counts[frame] += count
                    seen.add(frame)
for func, c in sorted(counts.items(), key=lambda x: -x[1])[:50]:
    print(f'{100.0*c/total:.2f}%  {func}')
\"" > "$OUTPUT_DIR/perf_steady_inclusive.txt" 2>/dev/null

$SCP $SERVER:/tmp/collapsed_steady.txt "$OUTPUT_DIR/collapsed_steady.txt" 2>/dev/null
$SCP $SERVER:/tmp/flamegraph_steady.svg "$OUTPUT_DIR/flamegraph_steady.svg" 2>/dev/null

echo "  Done: $OUTPUT_DIR/flamegraph_steady.svg"
echo ""

# ============================================================================
# Step 6: Collect final stats
# ============================================================================

echo "[6/8] Collecting final stats..."

RELAY_MEM=$($SSH $SERVER "ps -o rss= -p $RELAY_PID 2>/dev/null | xargs" || echo "0")
RELAY_MEM_MB=$(echo "scale=1; $RELAY_MEM / 1024" | bc 2>/dev/null || echo "?")

$SSH $SERVER "pkill moqx || true" 2>/dev/null
echo "  Relay RSS: ${RELAY_MEM_MB} MB"
echo ""

# ============================================================================
# Step 7: Analyze and categorize CPU
# ============================================================================

echo "[7/8] Analyzing CPU breakdown..."

categorize_cpu() {
    local FILE=$1
    local PREFIX=$2

    TOPN=$(grep -iE "(SimpleTopN|TopNFilter|TopNSubgroup|onSnapshot|recompute|topNBound|computeTopN)" "$FILE" | awk -F'%' '{sum += $1} END {printf "%.2f", sum}')
    TRANSPORT=$(grep -iE "(quic::|quinn|QuicStream|QuicTransport|BufQueue|ChainedByte|HTTPPriority|sendAck|QuicPathManager|BbrCongestion|IOBufQuicBatch)" "$FILE" | awk -F'%' '{sum += $1} END {printf "%.2f", sum}')
    MOQT=$(grep -iE "(moxygen::|MoQSession|MoQForwarder|writeVarint|proxygen::)" "$FILE" | awk -F'%' '{sum += $1} END {printf "%.2f", sum}')
    ALLOC=$(grep -iE "(malloc|cfree|realloc|__libc_malloc)" "$FILE" | awk -F'%' '{sum += $1} END {printf "%.2f", sum}')
    ATOMICS=$(grep -iE "(ldadd|ldset|cas|atomic)" "$FILE" | awk -F'%' '{sum += $1} END {printf "%.2f", sum}')
    FOLLY=$(grep -iE "(folly::)" "$FILE" | grep -viE "(quic|moxygen|SimpleTopN)" | awk -F'%' '{sum += $1} END {printf "%.2f", sum}')

    eval "${PREFIX}_TOPN='$TOPN'"
    eval "${PREFIX}_TRANSPORT='$TRANSPORT'"
    eval "${PREFIX}_MOQT='$MOQT'"
    eval "${PREFIX}_ALLOC='$ALLOC'"
    eval "${PREFIX}_ATOMICS='$ATOMICS'"
    eval "${PREFIX}_FOLLY='$FOLLY'"
}

categorize_cpu "$OUTPUT_DIR/perf_full_self.txt" "FULL"
categorize_cpu "$OUTPUT_DIR/perf_steady_self.txt" "STEADY"

# Inclusive top-N
FULL_TOPN_INCL=$(grep -iE "onSnapshotChanged" "$OUTPUT_DIR/perf_full_inclusive.txt" | head -1 | awk '{print $1}' | tr -d '%' || echo "0")
STEADY_TOPN_INCL=$(grep -iE "onSnapshotChanged" "$OUTPUT_DIR/perf_steady_inclusive.txt" | head -1 | awk -F'%' '{print $1}' || echo "0")

echo "  Full:         TopN=${FULL_TOPN}% Transport=${FULL_TRANSPORT}% MoQT=${FULL_MOQT}%"
echo "  Steady-State: TopN=${STEADY_TOPN}% Transport=${STEADY_TRANSPORT}% MoQT=${STEADY_MOQT}%"
echo ""

# ============================================================================
# Step 8: Generate comprehensive report
# ============================================================================

echo "[8/8] Generating comprehensive report..."

# Parse test results
OBJ_PUB=$(grep "Objects Published" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+' | tail -1 || echo "?")
OBJ_RCV=$(grep "Objects Received" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+' | tail -1 || echo "?")
SELF_RCV=$(grep "Self-Received" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+' | tail -1 || echo "?")
FWD_ERR=$(grep "Forward Errors" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+' | tail -1 || echo "?")
MSG_RATE=$(grep "Message Rate" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+\.[0-9]+' | tail -1 || echo "?")
STATUS=$(grep "Overall Status" "$OUTPUT_DIR/test_output.txt" | grep -oE '(PASSED|FAILED)' | tail -1 || echo "?")
TEST_DUR=$(grep "Test Duration" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+\.[0-9]+' | tail -1 || echo "?")

# Speech stats
SPEECH_STARTS=$(grep "Speech Starts" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+' | tail -1 || echo "?")
SPEECH_PER_PANELIST=$(grep "Avg Speeches/Panelist" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+\.[0-9]+' | tail -1 || echo "?")
SPEECH_TICKS=$(grep "Speech Ticks" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+' | head -1 || echo "?")
SILENT_TICKS=$(grep "Silent Ticks" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+' | head -1 || echo "?")
SPEECH_PCT=$(grep "Speech Ticks" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+\.[0-9]+%' || echo "?")
AVG_SPEECH_DUR=$(grep "Avg Speech Duration" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+' | tail -1 || echo "?")
AVG_SILENCE_DUR=$(grep "Avg Silence Duration" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+' | tail -1 || echo "?")

# Sysinfo
SYS_HOSTNAME=$(grep hostname "$OUTPUT_DIR/sysinfo.txt" | cut -d: -f2 | xargs)
SYS_KERNEL=$(grep kernel "$OUTPUT_DIR/sysinfo.txt" | cut -d: -f2 | xargs)
SYS_ARCH=$(grep "^arch:" "$OUTPUT_DIR/sysinfo.txt" | cut -d: -f2 | xargs)
SYS_CPUS=$(grep "^cpus:" "$OUTPUT_DIR/sysinfo.txt" | cut -d: -f2 | xargs)
SYS_CPU_MODEL=$(grep cpu_model "$OUTPUT_DIR/sysinfo.txt" | cut -d: -f2- | xargs)
SYS_MEM=$(grep memory_total "$OUTPUT_DIR/sysinfo.txt" | cut -d: -f2 | xargs)

# Active speakers calculation
# With speech model: avg speech 2-8s (avg 5s), avg silence 1-5s (avg 3s)
# Duty cycle = speech/(speech+silence) = 5/8 = 62.5%
# Expected concurrent speakers = 90 * 0.625 ≈ 56
# But the model has speech_start(300ms) + speaking(2-8s) so effective speech = 2.3-8.3s
# Active at any instant ≈ panelists * speech_fraction

cat > "$OUTPUT_DIR/report.md" << 'REPORT_HEREDOC'
# moqx Top-N Comprehensive Performance Report

REPORT_HEREDOC

cat >> "$OUTPUT_DIR/report.md" << REPORT
**Generated:** $(date '+%Y-%m-%d %H:%M:%S')
**Test:** ${TEST_NAME}

---

## 1. System Configuration

| Parameter | Value |
|-----------|-------|
| Hostname | ${SYS_HOSTNAME} |
| Kernel | ${SYS_KERNEL} |
| Architecture | ${SYS_ARCH} |
| CPUs | ${SYS_CPUS} |
| CPU Model | ${SYS_CPU_MODEL} |
| Total Memory | ${SYS_MEM} |

## 2. Test Configuration

| Parameter | Value |
|-----------|-------|
| Panelists (pub-sub) | ${PANELISTS} |
| Panelist Top-N | ${PANELIST_TOPN:-$TOP_N} |
| Pure Subscribers | ${SUBSCRIBERS} |
| Pure Sub Top-N | Mixed [${MIXED_TOPN:-$TOP_N}] |
| Total Connections | ${TOTAL_CONN} |
| Update Rate | ${UPDATE_HZ} Hz |
| Duration | ${DURATION}s |
| Warmup (excluded for steady-state) | ${WARMUP_SECS}s |
| Speech Mode | Yes |
| Perf Sampling Rate | ${PERF_HZ} Hz |

## 3. Test Results

| Metric | Value |
|--------|-------|
| **Overall Status** | **${STATUS}** |
| Test Duration | ${TEST_DUR}s |
| Objects Published | ${OBJ_PUB} |
| Objects Received | ${OBJ_RCV} |
| Self-Received (errors) | ${SELF_RCV} |
| Forward Errors | ${FWD_ERR} |
| **Message Rate** | **${MSG_RATE} msg/s** |
| Relay RSS | ${RELAY_MEM_MB} MB |

## 4. Speech Simulation Statistics

### Algorithm

The speech simulator uses a state machine:

\`\`\`
SILENT (1-5s random) → SPEECH_START (300ms, value=2) → SPEAKING (2-8s random, value=1) → ENDED → SILENT (value=0)
\`\`\`

- **Speech start value:** 2 (triggers ranking promotion)
- **Speaking value:** 1 (maintains position)
- **Silent value:** 0 (drops in ranking)
- **Transition probability:** Deterministic per-panelist based on independent timers
- **Expected duty cycle:** ~62% speaking (avg 5s speech / avg 8s cycle)
- **Expected concurrent speakers:** ~${PANELISTS} × 0.62 ≈ $((PANELISTS * 62 / 100)) at any instant

### Measured Statistics

| Metric | Value |
|--------|-------|
| Total Speech Starts | ${SPEECH_STARTS} |
| Avg Speeches per Panelist | ${SPEECH_PER_PANELIST} |
| Speech Ticks | ${SPEECH_TICKS} (${SPEECH_PCT}) |
| Silent Ticks | ${SILENT_TICKS} |
| Avg Speech Duration | ${AVG_SPEECH_DUR} ms |
| Avg Silence Duration | ${AVG_SILENCE_DUR} ms |

### Key Insight

At any given moment, ~62% of panelists are "speaking" (value > 0).
Ranking changes happen at speech start/end boundaries — not every tick.
With value deduplication (same value → no dirty flag), actual snapshot rebuilds
only happen when speakers change, not on every tick.

## 5. CPU Profile — Full Duration (Including Warmup)

This includes the connection establishment phase (first ~${WARMUP_SECS}s) where sessions
connect, subscribe, and initial selections are computed.

### By Category (Self-Time)

| Category | Self % | Description |
|----------|-------:|-------------|
| **Top-N Ranking** | **${FULL_TOPN}%** | Snapshot rebuild, boundary check, selection delta |
| QUIC Transport | ${FULL_TRANSPORT}% | Stream management, buffer ops, ACKs |
| MoQT Protocol | ${FULL_MOQT}% | Encode/decode, session forwarding |
| Memory Allocation | ${FULL_ALLOC}% | malloc/free |
| Atomics/Refcount | ${FULL_ATOMICS}% | shared_ptr atomic ops |
| Folly Utilities | ${FULL_FOLLY}% | F14 hash, futures, event loop |

### Top-N Inclusive Time

| Metric | Value |
|--------|-------|
| **onSnapshotChanged (inclusive)** | **${FULL_TOPN_INCL}%** |

### Top Functions (Self-Time)

\`\`\`
$(head -20 "$OUTPUT_DIR/perf_full_self.txt")
\`\`\`

### Flamegraph

See: [flamegraph_full.svg](flamegraph_full.svg)

Searching tips for the SVG:
- Search "TopN" or "SimpleTopN" to find ranking-related frames
- Search "quic::" for transport overhead
- Search "moxygen" for MoQT protocol handling
- Click any frame to zoom in; click "Reset Zoom" to restore

## 6. CPU Profile — Steady State Only (Excluding First ${WARMUP_SECS}s)

This represents pure forwarding performance after all connections are established
and the system has reached equilibrium.

### By Category (Self-Time)

| Category | Self % | Description |
|----------|-------:|-------------|
| **Top-N Ranking** | **${STEADY_TOPN}%** | Snapshot rebuild, boundary check, selection delta |
| QUIC Transport | ${STEADY_TRANSPORT}% | Stream management, buffer ops, ACKs |
| MoQT Protocol | ${STEADY_MOQT}% | Encode/decode, session forwarding |
| Memory Allocation | ${STEADY_ALLOC}% | malloc/free |
| Atomics/Refcount | ${STEADY_ATOMICS}% | shared_ptr atomic ops |
| Folly Utilities | ${STEADY_FOLLY}% | F14 hash, futures, event loop |

### Top-N Inclusive Time (Steady State)

| Metric | Value |
|--------|-------|
| **onSnapshotChanged (inclusive)** | **${STEADY_TOPN_INCL}%** |

### Top Functions (Self-Time, Steady State)

\`\`\`
$(head -20 "$OUTPUT_DIR/perf_steady_self.txt")
\`\`\`

### Flamegraph

See: [flamegraph_steady.svg](flamegraph_steady.svg)

## 7. Full vs Steady-State Comparison

| Category | Full Duration | Steady State | Delta |
|----------|:------------:|:------------:|:-----:|
| **Top-N Ranking** | ${FULL_TOPN}% | ${STEADY_TOPN}% | $(echo "$STEADY_TOPN - $FULL_TOPN" | bc 2>/dev/null || echo "?")% |
| QUIC Transport | ${FULL_TRANSPORT}% | ${STEADY_TRANSPORT}% | $(echo "$STEADY_TRANSPORT - $FULL_TRANSPORT" | bc 2>/dev/null || echo "?")% |
| MoQT Protocol | ${FULL_MOQT}% | ${STEADY_MOQT}% | $(echo "$STEADY_MOQT - $FULL_MOQT" | bc 2>/dev/null || echo "?")% |
| Memory Alloc | ${FULL_ALLOC}% | ${STEADY_ALLOC}% | $(echo "$STEADY_ALLOC - $FULL_ALLOC" | bc 2>/dev/null || echo "?")% |
| Atomics | ${FULL_ATOMICS}% | ${STEADY_ATOMICS}% | $(echo "$STEADY_ATOMICS - $FULL_ATOMICS" | bc 2>/dev/null || echo "?")% |
| **Top-N Inclusive** | ${FULL_TOPN_INCL}% | ${STEADY_TOPN_INCL}% | $(echo "$STEADY_TOPN_INCL - $FULL_TOPN_INCL" | bc 2>/dev/null || echo "?")% |

**Interpretation:**
- If steady-state Top-N is lower than full: the warmup/connection phase has higher
  ranking cost (initial selections for all sessions).
- If steady-state Transport is higher: transport dominates when no new connections
  are being established.
- Top-N inclusive includes: snapshot rebuild, boundary check, per-session delta
  computation, select/evict callback overhead.

## 8. Measurement Methodology

### Profiling Approach

1. **Tool:** Linux \`perf record\` with DWARF call graphs at ${PERF_HZ} Hz sampling rate
2. **Target:** Single-process relay (PID-targeted profiling, no system-wide noise)
3. **Duration:** Full test run (${DURATION}s) captured in one recording
4. **Steady-state extraction:** \`perf script --time ${WARMUP_SECS}.0,${PERF_WINDOW}.0\` filters
   samples to only those collected after the warmup window

### What Each Measurement Means

| Metric | Meaning |
|--------|---------|
| Self-Time % | CPU time spent IN this function (excluding callees) |
| Inclusive % | CPU time spent in this function OR any function it calls |
| Category % | Sum of self-time for all functions in that category |

### Flamegraph Reading Guide

- **Width** = proportion of total CPU samples (wider = more CPU time)
- **Y-axis** = call depth (bottom = entry point, top = leaf functions)
- **Color** = arbitrary (no meaning, just visual distinction)
- **Search** (Ctrl+F in browser) highlights matching frames

### Steady-State Window

- First ${WARMUP_SECS}s excluded to remove connection establishment overhead
- During warmup: ${TOTAL_CONN} QUIC connections established, TLS handshakes, initial
  TRACK_FILTER subscriptions processed, first batch of selections computed
- After warmup: system in equilibrium with established connections forwarding objects

### Speech Simulation vs Real Workload

The speech simulator models a realistic audio conference:
- Panelists independently transition between speaking/silent states
- Ranking changes only happen at state boundaries (not every tick)
- Value deduplication: if value unchanged from previous tick, no dirty flag set
- This means actual ranking work is proportional to \`number of state changes/sec\`,
  NOT \`panelists × update_hz\`

### Potential Confounds

- Single-machine test (client + relay colocation effects minimized via separate processes)
- Relay runs single-threaded event loop (all work on one core)
- QUIC over localhost removes real network latency/loss effects
- \`perf\` sampling can slightly perturb timing-sensitive paths

## 9. Files

| File | Description |
|------|-------------|
| report.md | This report |
| flamegraph_full.svg | Full-duration interactive flamegraph |
| flamegraph_steady.svg | Steady-state-only interactive flamegraph |
| collapsed_full.txt | Full-duration collapsed stacks |
| collapsed_steady.txt | Steady-state collapsed stacks |
| perf_full_self.txt | Full self-time profile |
| perf_full_inclusive.txt | Full inclusive-time profile |
| perf_steady_self.txt | Steady-state self-time profile |
| perf_steady_inclusive.txt | Steady-state inclusive-time profile |
| test_output.txt | Full load test output |
| sysinfo.txt | System information |
REPORT

echo ""
echo "================================================================================"
echo "  Done!"
echo "================================================================================"
echo ""
echo "  Report:         $OUTPUT_DIR/report.md"
echo "  Full SVG:       $OUTPUT_DIR/flamegraph_full.svg"
echo "  Steady SVG:     $OUTPUT_DIR/flamegraph_steady.svg"
echo "  Status:         $STATUS"
echo "  Msg Rate:       $MSG_RATE msg/s"
echo "  TopN Self:      Full=${FULL_TOPN}%  Steady=${STEADY_TOPN}%"
echo "  TopN Inclusive:  Full=${FULL_TOPN_INCL}%  Steady=${STEADY_TOPN_INCL}%"
echo ""

if [ "$OPEN" = true ]; then
    open "$OUTPUT_DIR/flamegraph_full.svg" 2>/dev/null || true
    open "$OUTPUT_DIR/flamegraph_steady.svg" 2>/dev/null || true
    open "$OUTPUT_DIR/report.md" 2>/dev/null || true
fi
