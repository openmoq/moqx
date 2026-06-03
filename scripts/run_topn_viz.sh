#!/bin/bash
# run_topn_viz.sh — Run a small Top-N load test on the remote server,
# capture TOPN_EVENT logs, generate interactive HTML visualization locally.
#
# Usage:
#   ./scripts/run_topn_viz.sh                    # defaults: 6 panelists, 4 subs, top-3, 20s
#   ./scripts/run_topn_viz.sh -p 10 -s 10 -n 5  # custom
#   ./scripts/run_topn_viz.sh -d 60              # longer test

set -e

# Defaults
PANELISTS=6
SUBSCRIBERS=4
TOP_N=3
DURATION=20
SERVER="admin@snk-dev-1.m10x.org"
SSH_KEY="$HOME/.ssh/keys/snk-dev-server.pem"
SSH="ssh -i $SSH_KEY -o IdentitiesOnly=yes -o ConnectTimeout=10"
SCP="scp -i $SSH_KEY -o IdentitiesOnly=yes"
OPEN=true

usage() {
    echo "Usage: $0 [-p panelists] [-s subscribers] [-n top_n] [-d duration] [--no-open]"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case $1 in
        -p) PANELISTS=$2; shift 2;;
        -s) SUBSCRIBERS=$2; shift 2;;
        -n) TOP_N=$2; shift 2;;
        -d) DURATION=$2; shift 2;;
        --no-open) OPEN=false; shift;;
        -h|--help) usage;;
        *) echo "Unknown option: $1"; usage;;
    esac
done

echo "=== Top-N Visualization ==="
echo "  Panelists: $PANELISTS, Subscribers: $SUBSCRIBERS, Top-N: $TOP_N, Duration: ${DURATION}s"
echo ""

# 1. Kill any existing relay
echo "[1/5] Stopping existing relay..."
$SSH $SERVER "pkill moqx || true" 2>/dev/null

sleep 1

# 2. Start relay with event logging
echo "[2/5] Starting relay with TOPN_EVENT logging..."
$SSH $SERVER "rm -f /tmp/topn_events.log"
$SSH -f $SERVER "nohup ~/moqx/build/moqx --config ~/moqx/config.example.yaml --topn_event_log /tmp/topn_events.log > /tmp/relay_viz.log 2>&1 &"
sleep 2

# Verify relay is running
if ! $SSH $SERVER "pgrep moqx > /dev/null 2>&1"; then
    echo "ERROR: Relay failed to start. Check /tmp/relay_viz.log on server."
    exit 1
fi
echo "  Relay running."

# 3. Run load test
echo "[3/5] Running load test (${DURATION}s)..."
$SSH -o ServerAliveInterval=30 $SERVER \
    "~/moqx/build/test/track_filter_load_test \
        --relay_url 'https://localhost:9668/moq-relay' --insecure true \
        --panelists $PANELISTS --subscribers $SUBSCRIBERS \
        --top_n $TOP_N --speech_mode true --duration $DURATION 2>&1" \
    | grep -E "(Objects|Self-Received|Message Rate|Overall Status|PASSED|FAILED|Correctness)" \
    || true

echo ""

# 4. Fetch event log
echo "[4/5] Fetching event log..."
$SCP $SERVER:/tmp/topn_events.log /tmp/topn_events.log
EVENTS=$(wc -l < /tmp/topn_events.log | tr -d ' ')
echo "  Downloaded $EVENTS events."

# 5. Generate visualization
echo "[5/5] Generating HTML..."
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT="/tmp/topn_timeline_${PANELISTS}p_${SUBSCRIBERS}s_n${TOP_N}.html"
python3 "$SCRIPT_DIR/tools/topn_viz.py" /tmp/topn_events.log -o "$OUTPUT"

# Kill relay
$SSH $SERVER "pkill moqx || true" 2>/dev/null

echo ""
echo "=== Done ==="
echo "  Output: $OUTPUT"

if [ "$OPEN" = true ]; then
    open "$OUTPUT" 2>/dev/null || xdg-open "$OUTPUT" 2>/dev/null || echo "  Open $OUTPUT in your browser."
fi
