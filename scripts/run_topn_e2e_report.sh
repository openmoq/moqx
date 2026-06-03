#!/bin/bash
# run_topn_e2e_report.sh — Run full E2E perf test on remote server with perf profiling,
# generate flamegraph SVGs, collect system info, and produce a markdown report.
#
# Usage:
#   ./scripts/run_topn_e2e_report.sh -p 80 -s 800 -n 45
#   ./scripts/run_topn_e2e_report.sh -p 80 -s 800 --mixed "1,5,10,25,45,65,80"
#   ./scripts/run_topn_e2e_report.sh -p 80 -s 800 -n 45 --hz 6 -d 120
#
# Outputs (in ./perf_reports/<timestamp>/):
#   - report.md              Full markdown report
#   - flamegraph.svg         Interactive flamegraph
#   - collapsed.txt          Collapsed stacks for further analysis
#   - test_output.txt        Raw load test output
#   - perf_self.txt          Self-time top functions
#   - perf_inclusive.txt     Inclusive-time top functions

set -e

# ============================================================================
# Configuration
# ============================================================================

PANELISTS=80
SUBSCRIBERS=800
TOP_N=45
MIXED_TOPN=""
PANELIST_TOPN=""
DURATION=60
UPDATE_HZ=30
SERVER="admin@snk-dev-1.m10x.org"
SSH_KEY="$HOME/.ssh/keys/snk-dev-server.pem"
SSH="ssh -i $SSH_KEY -o IdentitiesOnly=yes -o ConnectTimeout=10 -o ServerAliveInterval=30"
SCP="scp -i $SSH_KEY -o IdentitiesOnly=yes"
PERF_HZ=999
OPEN=true

usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  -p NUM        Number of panelists/pub-subscribers (default: 80)"
    echo "  -s NUM        Number of pure subscribers (default: 800)"
    echo "  -n NUM        Top-N value (default: 45)"
    echo "  --mixed STR   Comma-separated N values for pure subs (overrides -n)"
    echo "  --panelist-n  Top-N for panelists (default: same as subscribers)"
    echo "  --hz NUM      Update frequency in Hz (default: 30)"
    echo "  -d NUM        Test duration in seconds (default: 60)"
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
        --no-open) OPEN=false; shift;;
        -h|--help) usage;;
        *) echo "Unknown option: $1"; usage;;
    esac
done

# ============================================================================
# Setup output directory
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TIMESTAMP=$(date +%Y%m%d-%H%M%S)

if [ -n "$MIXED_TOPN" ]; then
    TEST_NAME="mixed_${PANELISTS}p_${SUBSCRIBERS}s_${UPDATE_HZ}hz"
else
    TEST_NAME="n${TOP_N}_${PANELISTS}p_${SUBSCRIBERS}s_${UPDATE_HZ}hz"
fi

OUTPUT_DIR="${SCRIPT_DIR}/perf_reports/${TIMESTAMP}_${TEST_NAME}"
mkdir -p "$OUTPUT_DIR"

echo "================================================================================"
echo "  moqx Top-N E2E Performance Report"
echo "================================================================================"
echo ""
echo "  Configuration:"
echo "    Panelists:    $PANELISTS"
echo "    Subscribers:  $SUBSCRIBERS"
if [ -n "$MIXED_TOPN" ]; then
    echo "    Top-N:        Mixed [$MIXED_TOPN]"
else
    echo "    Top-N:        $TOP_N"
fi
echo "    Update Hz:    $UPDATE_HZ"
echo "    Duration:     ${DURATION}s"
echo "    Server:       $SERVER"
echo ""
echo "  Output:         $OUTPUT_DIR"
echo ""

# ============================================================================
# Step 1: Collect system information
# ============================================================================

echo "[1/7] Collecting system information..."

SYSINFO=$($SSH $SERVER 'echo "hostname: $(hostname)"; echo "kernel: $(uname -r)"; echo "arch: $(uname -m)"; echo "cpus: $(nproc)"; echo "cpu_model: $(grep -m1 "model name\|Hardware" /proc/cpuinfo | cut -d: -f2 | xargs)"; echo "memory_total: $(free -h | awk "/Mem:/{print \$2}")"; echo "memory_avail: $(free -h | awk "/Mem:/{print \$7}")"; echo "ulimit_nofile: $(ulimit -n)"; echo "perf_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid)"')

echo "$SYSINFO" > "$OUTPUT_DIR/sysinfo.txt"
echo "  $(echo "$SYSINFO" | grep hostname)"
echo "  $(echo "$SYSINFO" | grep cpus)"
echo "  $(echo "$SYSINFO" | grep memory_total)"
echo ""

# ============================================================================
# Step 2: Start relay
# ============================================================================

echo "[2/7] Starting relay..."

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
# Steps 3-5: Run perf + load test in one session, then generate reports
# ============================================================================

echo "[3/7] Running perf record + load test (${DURATION}s)..."

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

PERF_WINDOW=$((DURATION + 10))

$SSH $SERVER "ulimit -n 65536; sudo perf record -F $PERF_HZ -p $RELAY_PID -g -o /tmp/perf_e2e.data -- sleep $PERF_WINDOW &
PERF_PID=\$!
sleep 2
~/moqx/build/test/track_filter_load_test $LOAD_TEST_ARGS 2>&1
wait \$PERF_PID 2>/dev/null || true" > "$OUTPUT_DIR/test_output.txt"

# Extract key metrics
grep -E "(Objects Published|Objects Received|Self-Received|Forward Errors|Message Rate|Overall Status|Test Duration|Total Messages)" "$OUTPUT_DIR/test_output.txt" | sed 's/^  /    /' || true
echo ""

echo "[5/7] Generating flamegraph and perf reports..."

# Generate collapsed stacks
$SSH $SERVER "sudo perf script -i /tmp/perf_e2e.data 2>/dev/null | ~/FlameGraph/stackcollapse-perf.pl > /tmp/collapsed_e2e.txt 2>/dev/null"

# Generate flamegraph SVG
TITLE="moqx relay — ${TEST_NAME} (${DURATION}s)"
$SSH $SERVER "cat /tmp/collapsed_e2e.txt | ~/FlameGraph/flamegraph.pl --title '$TITLE' --width 1400 > /tmp/flamegraph_e2e.svg 2>/dev/null"

# Get perf reports
$SSH $SERVER 'sudo perf report -i /tmp/perf_e2e.data --stdio --no-children --sort=symbol --percent-limit=0.3 2>/dev/null | grep -E "^\s+[0-9]+\.[0-9]+%" | sed "s/^\s\+//; s/\s\+\[.\]\s\+/  /" | sed "s/\s\+-\s\+-\s*$//"' > "$OUTPUT_DIR/perf_self.txt"

$SSH $SERVER 'sudo perf report -i /tmp/perf_e2e.data --stdio --children --sort=symbol --percent-limit=0.3 2>/dev/null | grep -E "^\s+[0-9]+\.[0-9]+%\s+[0-9]" | sed "s/^\s\+//; s/\s\+\[.\]\s\+/  /" | sed "s/\s\+-\s\+-\s*$//"' > "$OUTPUT_DIR/perf_inclusive.txt"

# Download artifacts
$SCP $SERVER:/tmp/collapsed_e2e.txt "$OUTPUT_DIR/collapsed.txt" 2>/dev/null
$SCP $SERVER:/tmp/flamegraph_e2e.svg "$OUTPUT_DIR/flamegraph.svg" 2>/dev/null

echo "  Flamegraph: $OUTPUT_DIR/flamegraph.svg"
echo ""

# ============================================================================
# Step 6: Kill relay and collect memory info
# ============================================================================

echo "[6/7] Collecting final stats..."

# Get relay memory usage before killing
RELAY_MEM=$($SSH $SERVER "ps -o rss= -p $RELAY_PID 2>/dev/null | xargs" || echo "0")
RELAY_MEM_MB=$(echo "scale=1; $RELAY_MEM / 1024" | bc 2>/dev/null || echo "?")

$SSH $SERVER "pkill moqx || true" 2>/dev/null
echo "  Relay RSS: ${RELAY_MEM_MB} MB"
echo ""

# ============================================================================
# Step 7: Generate markdown report
# ============================================================================

echo "[7/7] Generating report..."

# Parse test results
OBJ_PUB=$(grep "Objects Published" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+' | tail -1 || echo "?")
OBJ_RCV=$(grep "Objects Received" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+' | tail -1 || echo "?")
SELF_RCV=$(grep "Self-Received" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+' | tail -1 || echo "?")
FWD_ERR=$(grep "Forward Errors" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+' | tail -1 || echo "?")
MSG_RATE=$(grep "Message Rate" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+\.[0-9]+' | tail -1 || echo "?")
STATUS=$(grep "Overall Status" "$OUTPUT_DIR/test_output.txt" | grep -oE '(PASSED|FAILED)' | tail -1 || echo "?")
TEST_DUR=$(grep "Test Duration" "$OUTPUT_DIR/test_output.txt" | grep -oE '[0-9]+\.[0-9]+' | tail -1 || echo "?")

# Categorize perf results
TOPN_SELF=$(grep -iE "(SimpleTopN|TopNFilter|TopNSubgroup|onSnapshot|recompute|topNBound|computeTopN)" "$OUTPUT_DIR/perf_self.txt" | awk '{sum += $1} END {printf "%.2f", sum}' || echo "0")
TOPN_INCL=$(grep -iE "onSnapshotChanged" "$OUTPUT_DIR/perf_inclusive.txt" | head -1 | awk '{print $1}' | tr -d '%' || echo "0")
TRANSPORT_SELF=$(grep -iE "(quic::|quinn|QuicStream|QuicTransport|BufQueue|ChainedByte|HTTPPriority|sendAck)" "$OUTPUT_DIR/perf_self.txt" | awk '{sum += $1} END {printf "%.2f", sum}' || echo "0")
ALLOC_SELF=$(grep -iE "^[0-9].*\s(malloc|cfree|realloc|__libc_malloc)" "$OUTPUT_DIR/perf_self.txt" | awk '{sum += $1} END {printf "%.2f", sum}' || echo "0")
MOQT_SELF=$(grep -iE "(moxygen::|MoQSession|MoQForwarder|writeVarint)" "$OUTPUT_DIR/perf_self.txt" | awk '{sum += $1} END {printf "%.2f", sum}' || echo "0")
ATOMICS_SELF=$(grep -iE "(ldadd|ldset|cas|atomic)" "$OUTPUT_DIR/perf_self.txt" | awk '{sum += $1} END {printf "%.2f", sum}' || echo "0")

# Parse sysinfo
SYS_HOSTNAME=$(grep hostname "$OUTPUT_DIR/sysinfo.txt" | cut -d: -f2 | xargs)
SYS_KERNEL=$(grep kernel "$OUTPUT_DIR/sysinfo.txt" | cut -d: -f2 | xargs)
SYS_ARCH=$(grep "^arch:" "$OUTPUT_DIR/sysinfo.txt" | cut -d: -f2 | xargs)
SYS_CPUS=$(grep "^cpus:" "$OUTPUT_DIR/sysinfo.txt" | cut -d: -f2 | xargs)
SYS_CPU_MODEL=$(grep cpu_model "$OUTPUT_DIR/sysinfo.txt" | cut -d: -f2- | xargs)
SYS_MEM=$(grep memory_total "$OUTPUT_DIR/sysinfo.txt" | cut -d: -f2 | xargs)
SYS_ULIMIT=$(grep ulimit "$OUTPUT_DIR/sysinfo.txt" | cut -d: -f2 | xargs)

# Top-N specific functions
TOPN_FUNCS=$(grep -iE "(SimpleTopN|TopNFilter|TopNSubgroup|onSnapshot|recompute|topNBound|computeTopN|F14Table.*FullTrack)" "$OUTPUT_DIR/perf_self.txt" | head -10)

# Transport functions
TRANSPORT_FUNCS=$(grep -iE "(quic::|QuicStream|QuicTransport|BufQueue|ChainedByte)" "$OUTPUT_DIR/perf_self.txt" | head -10)

# Write report
cat > "$OUTPUT_DIR/report.md" << REPORT
# Top-N E2E Performance Report

**Generated:** $(date '+%Y-%m-%d %H:%M:%S')
**Test:** ${TEST_NAME}

---

## System Configuration

| Parameter | Value |
|-----------|-------|
| Hostname | ${SYS_HOSTNAME} |
| Kernel | ${SYS_KERNEL} |
| Architecture | ${SYS_ARCH} |
| CPUs | ${SYS_CPUS} |
| CPU Model | ${SYS_CPU_MODEL} |
| Total Memory | ${SYS_MEM} |
| ulimit -n | ${SYS_ULIMIT} |

## Test Configuration

| Parameter | Value |
|-----------|-------|
| Panelists (pub-sub) | ${PANELISTS} |
| Pure Subscribers | ${SUBSCRIBERS} |
| Total Connections | $((PANELISTS + SUBSCRIBERS)) |
$(if [ -n "$MIXED_TOPN" ]; then echo "| Top-N | Mixed [${MIXED_TOPN}] |"; else echo "| Top-N | ${TOP_N} |"; fi)
| Update Rate | ${UPDATE_HZ} Hz |
| Duration | ${DURATION}s |
| Speech Mode | Yes (random speakers, p=0.03/tick) |
| Perf Sampling | ${PERF_HZ} Hz |

## Test Results

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

## CPU Profile Summary

### By Category (Self-Time)

| Category | Self % | Description |
|----------|-------:|-------------|
| **Top-N Ranking** | **${TOPN_SELF}%** | Snapshot rebuild, boundary check, selection delta |
| QUIC Transport | ${TRANSPORT_SELF}% | Stream management, buffer ops, ACKs |
| MoQT Protocol | ${MOQT_SELF}% | Encode/decode, forwarding logic |
| Memory Allocation | ${ALLOC_SELF}% | malloc/free |
| Atomics | ${ATOMICS_SELF}% | Atomic refcount ops (shared_ptr) |

### Top-N Inclusive Time

| Metric | Value |
|--------|-------|
| **onSnapshotChanged (inclusive)** | **${TOPN_INCL}%** |

This is the total CPU cost of the ranking path including all callees
(boundary check, delta computation, select/evict callbacks).

### Top-N Functions (Self-Time)

\`\`\`
$(echo "$TOPN_FUNCS" | head -10)
\`\`\`

### Transport Functions (Self-Time)

\`\`\`
$(echo "$TRANSPORT_FUNCS" | head -10)
\`\`\`

### Full Self-Time Profile (top 25)

\`\`\`
$(head -25 "$OUTPUT_DIR/perf_self.txt")
\`\`\`

## Flamegraph

See: [flamegraph.svg](flamegraph.svg)

## Files

| File | Description |
|------|-------------|
| report.md | This report |
| flamegraph.svg | Interactive flamegraph (open in browser) |
| collapsed.txt | Collapsed stacks for custom analysis |
| test_output.txt | Full load test output |
| perf_self.txt | All functions by self-time |
| perf_inclusive.txt | All functions by inclusive time |
| sysinfo.txt | System information |
REPORT

echo ""
echo "================================================================================"
echo "  Done!"
echo "================================================================================"
echo ""
echo "  Report:     $OUTPUT_DIR/report.md"
echo "  Flamegraph: $OUTPUT_DIR/flamegraph.svg"
echo "  Status:     $STATUS"
echo "  Msg Rate:   $MSG_RATE msg/s"
echo "  TopN Self:  ${TOPN_SELF}%"
echo "  TopN Incl:  ${TOPN_INCL}%"
echo ""

if [ "$OPEN" = true ]; then
    open "$OUTPUT_DIR/flamegraph.svg" 2>/dev/null || xdg-open "$OUTPUT_DIR/flamegraph.svg" 2>/dev/null || true
fi
