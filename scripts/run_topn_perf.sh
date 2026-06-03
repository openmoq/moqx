#!/bin/bash
# Run Simple N+X Top-N performance test with analysis and visualization.
#
# Usage:
#   ./scripts/run_topn_perf.sh [BUILD_DIR]
#
# Options (via environment variables):
#   PANELISTS=50        Number of panelists (pub-subscribers)
#   SUBSCRIBERS=500     Number of pure subscribers
#   TOP_N=5             Top-N selection value
#   ROUNDS=5            Number of test rounds
#   DURATION_MS=10000   Duration per round (ms)
#   GROUP_MS=20         Group/update interval (ms)
#   READERS=4           Concurrent reader threads
#   OPEN_HTML=1         Open results in browser (default: 1)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${1:-${PROJECT_DIR}/build}"

# Configuration with defaults
PANELISTS="${PANELISTS:-50}"
SUBSCRIBERS="${SUBSCRIBERS:-500}"
TOP_N="${TOP_N:-5}"
ROUNDS="${ROUNDS:-5}"
DURATION_MS="${DURATION_MS:-10000}"
GROUP_MS="${GROUP_MS:-20}"
READERS="${READERS:-4}"
OPEN_HTML="${OPEN_HTML:-1}"

# Output paths
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
OUTPUT_DIR="${PROJECT_DIR}/perf_results"
mkdir -p "${OUTPUT_DIR}"

PERF_JSON="${OUTPUT_DIR}/topn-perf-${TIMESTAMP}.json"
EVENT_LOG="${OUTPUT_DIR}/topn-events-${TIMESTAMP}.log"
ANALYSIS_HTML="${OUTPUT_DIR}/topn-analysis-${TIMESTAMP}.html"
VIZ_HTML="${OUTPUT_DIR}/topn-viz-${TIMESTAMP}.html"

PERF_BINARY="${BUILD_DIR}/test/simple_topn_tracker_perf_test"

echo "================================================================================"
echo "  moqx Simple N+X Top-N Performance Test"
echo "================================================================================"
echo ""
echo "Configuration:"
echo "  Panelists:      ${PANELISTS}"
echo "  Subscribers:    ${SUBSCRIBERS}"
echo "  Top-N:          ${TOP_N}"
echo "  Rounds:         ${ROUNDS}"
echo "  Duration/round: ${DURATION_MS} ms"
echo "  Group interval: ${GROUP_MS} ms"
echo "  Readers:        ${READERS}"
echo ""

# Step 1: Check binary exists (or build)
if [ ! -f "${PERF_BINARY}" ]; then
  echo "Binary not found at ${PERF_BINARY}"
  echo "Building..."
  if [ -f "${BUILD_DIR}/build.ninja" ]; then
    ninja -C "${BUILD_DIR}" simple_topn_tracker_perf_test
  elif [ -f "${BUILD_DIR}/Makefile" ]; then
    make -C "${BUILD_DIR}" simple_topn_tracker_perf_test -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
  else
    echo "Error: No build system found in ${BUILD_DIR}. Run cmake first."
    exit 1
  fi
fi

echo "Step 1: Running performance test..."
echo ""

"${PERF_BINARY}" \
  --gtest_filter="SimpleTopNTrackerPerfTest.MultiRoundWebinarSimulation" \
  --perf_panelists="${PANELISTS}" \
  --perf_subscribers="${SUBSCRIBERS}" \
  --perf_top_n="${TOP_N}" \
  --perf_rounds="${ROUNDS}" \
  --perf_round_duration_ms="${DURATION_MS}" \
  --perf_group_interval_ms="${GROUP_MS}" \
  --perf_reader_threads="${READERS}" \
  --perf_output="${PERF_JSON}" \
  --perf_event_log="${EVENT_LOG}"

echo ""
echo "Step 2: Analyzing results..."
echo ""

python3 "${PROJECT_DIR}/tools/analyze_topn_perf.py" "${PERF_JSON}" --html "${ANALYSIS_HTML}"

echo ""
echo "Step 3: Generating visualization..."
echo ""

python3 "${PROJECT_DIR}/tools/topn_viz.py" "${EVENT_LOG}" -o "${VIZ_HTML}"

echo ""
echo "================================================================================"
echo "  Results"
echo "================================================================================"
echo ""
echo "  JSON data:     ${PERF_JSON}"
echo "  Event log:     ${EVENT_LOG}"
echo "  Analysis:      ${ANALYSIS_HTML}"
echo "  Visualization: ${VIZ_HTML}"
echo ""

if [ "${OPEN_HTML}" = "1" ]; then
  if command -v open &>/dev/null; then
    open "${ANALYSIS_HTML}"
    open "${VIZ_HTML}"
  elif command -v xdg-open &>/dev/null; then
    xdg-open "${ANALYSIS_HTML}"
    xdg-open "${VIZ_HTML}"
  fi
fi

echo "Done."
