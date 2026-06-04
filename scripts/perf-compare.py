#!/usr/bin/env python3
"""perf-compare.py — Compare current performance results against a rolling baseline.

Loads historical results from a JSON data directory, computes rolling averages,
compares the current run, and outputs a markdown summary suitable for PR comments.

Usage:
    scripts/perf-compare.py --current results.json --data-dir data/ [--window 10] [--threshold 5]

Exit code is always 0 (non-blocking). Regressions are flagged in the output only.
"""

import argparse
import json
import os
import sys
from pathlib import Path


# Metrics to track with display configuration.
# (key, display_name, unit, higher_is_better, threshold_pct)
TRACKED_METRICS = [
    ("peak_subscribers", "Peak Subscribers", "", True, 5),
    ("throughput_mbps", "Throughput", "Mbps", True, 5),
    ("throughput_per_core_mbps", "Throughput/Core", "Mbps", True, 5),
    ("subscribers_per_core", "Subscribers/Core", "", True, 5),
    ("delivery_success_pct", "Delivery Success", "%", True, 2),
    ("relay_cpu_pct", "Relay CPU", "%", False, 10),
    ("relay_rss_mb", "Relay RSS", "MB", False, 10),
    ("rss_per_session_kb", "RSS/Session", "KB", False, 10),
    ("total_resets", "Total Resets", "", False, 50),
    ("udp_errors_per_sec", "UDP Errors/s", "", False, 50),
]


def load_results_file(path: Path) -> dict:
    """Load a single results JSON file."""
    with open(path) as f:
        return json.load(f)


def load_baseline_data(data_dir: Path, branch: str, window: int) -> list[dict]:
    """Load the most recent N results from the data directory for a given branch."""
    results = []
    index_path = data_dir / "index.json"

    if index_path.exists():
        with open(index_path) as f:
            index = json.load(f)

        # Filter to same branch (typically 'main')
        entries = [e for e in index.get("runs", []) if e.get("branch") == branch]
        # Sort by timestamp descending
        entries.sort(key=lambda e: e.get("timestamp", ""), reverse=True)
        # Take most recent N
        entries = entries[:window]

        for entry in entries:
            data_file = data_dir / entry.get("file", "")
            if data_file.exists():
                try:
                    results.append(load_results_file(data_file))
                except (json.JSONDecodeError, KeyError):
                    continue
    else:
        # Fallback: scan directory for JSON files
        json_files = sorted(data_dir.glob("*.json"), key=os.path.getmtime, reverse=True)
        for jf in json_files[:window]:
            if jf.name == "index.json":
                continue
            try:
                data = load_results_file(jf)
                if data.get("branch") == branch:
                    results.append(data)
            except (json.JSONDecodeError, KeyError):
                continue

    return results


def compute_baseline(historical: list[dict]) -> dict:
    """Compute rolling average for each tracked metric."""
    if not historical:
        return {}

    baseline = {}
    for key, _, _, _, _ in TRACKED_METRICS:
        values = []
        for run in historical:
            val = run.get("results", {}).get(key)
            if val is not None:
                try:
                    values.append(float(val))
                except (ValueError, TypeError):
                    pass
        if values:
            baseline[key] = sum(values) / len(values)

    return baseline


def compare_results(current: dict, baseline: dict, threshold_override: float = None) -> list[dict]:
    """Compare current results against baseline, flagging regressions."""
    comparisons = []
    results = current.get("results", {})

    for key, display_name, unit, higher_is_better, default_threshold in TRACKED_METRICS:
        current_val = results.get(key)
        baseline_val = baseline.get(key)

        if current_val is None or baseline_val is None or baseline_val == 0:
            comparisons.append({
                "key": key,
                "name": display_name,
                "unit": unit,
                "current": current_val,
                "baseline": baseline_val,
                "delta_pct": None,
                "status": "no-data",
            })
            continue

        current_val = float(current_val)
        baseline_val = float(baseline_val)
        threshold = threshold_override if threshold_override is not None else default_threshold

        delta_pct = ((current_val - baseline_val) / baseline_val) * 100

        # Determine if this is a regression
        if higher_is_better:
            is_regression = delta_pct < -threshold
        else:
            is_regression = delta_pct > threshold

        status = "regression" if is_regression else "ok"

        comparisons.append({
            "key": key,
            "name": display_name,
            "unit": unit,
            "current": current_val,
            "baseline": baseline_val,
            "delta_pct": delta_pct,
            "status": status,
        })

    return comparisons


def format_value(val, unit: str) -> str:
    """Format a metric value for display."""
    if val is None:
        return "—"
    if isinstance(val, float):
        if val == int(val) and unit not in ("%", "Mbps", "MB", "KB"):
            return f"{int(val)}"
        return f"{val:.1f}"
    return str(val)


def format_delta(delta_pct, status: str) -> str:
    """Format a delta percentage with status indicator."""
    if delta_pct is None:
        return "—"
    sign = "+" if delta_pct >= 0 else ""
    indicator = ""
    if status == "regression":
        indicator = " :warning:"
    return f"{sign}{delta_pct:.1f}%{indicator}"


def generate_markdown(current: dict, comparisons: list[dict], window: int, baseline_count: int) -> str:
    """Generate markdown summary for PR comment."""
    lines = []

    commit_short = current.get("commit_short", current.get("commit", "")[:7])
    branch = current.get("branch", "unknown")

    has_regressions = any(c["status"] == "regression" for c in comparisons)
    has_baseline = baseline_count > 0

    if has_regressions:
        lines.append("## :zap: Performance Results :warning:")
    else:
        lines.append("## :zap: Performance Results")

    lines.append("")

    if has_baseline:
        lines.append(f"Comparing `{commit_short}` against rolling {baseline_count}-run average on `main`.")
    else:
        lines.append(f"Results for `{commit_short}` (no baseline data available yet).")

    lines.append("")

    # Summary table
    lines.append("| Metric | This Run | Baseline | Δ |")
    lines.append("|--------|----------|----------|---|")

    for comp in comparisons:
        if comp["status"] == "no-data" and comp["current"] is None:
            continue
        current_str = format_value(comp["current"], comp["unit"])
        if comp["unit"]:
            current_str += f" {comp['unit']}"
        baseline_str = format_value(comp["baseline"], comp["unit"])
        if comp["baseline"] is not None and comp["unit"]:
            baseline_str += f" {comp['unit']}"
        delta_str = format_delta(comp["delta_pct"], comp["status"])
        lines.append(f"| {comp['name']} | {current_str} | {baseline_str} | {delta_str} |")

    lines.append("")

    # Params detail
    params = current.get("params", {})
    test_result = current.get("results", {}).get("test_result", "UNKNOWN")
    lines.append("<details><summary>Test parameters & full results</summary>")
    lines.append("")
    lines.append(f"- **Result:** {test_result}")
    lines.append(f"- **Subscribers:** {params.get('subscriber_max', '?')} (ramp {params.get('ramp', '?')}/s)")
    lines.append(f"- **Duration:** {params.get('duration', '?')}s")
    lines.append(f"- **IO threads:** {params.get('io_threads', '?')}")
    lines.append(f"- **Client threads:** {params.get('client_threads', '?')}")
    lines.append(f"- **Transport:** {params.get('transport', '?')}")
    lines.append(f"- **Baseline window:** {window} runs ({baseline_count} available)")
    lines.append("")
    lines.append("</details>")
    lines.append("")

    if has_regressions:
        regressed = [c["name"] for c in comparisons if c["status"] == "regression"]
        lines.append(f":warning: **Potential regressions detected** in: {', '.join(regressed)}")
        lines.append("")

    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="Compare perf results against baseline")
    parser.add_argument("--current", required=True, help="Path to current results JSON")
    parser.add_argument("--data-dir", required=True, help="Path to historical data directory")
    parser.add_argument("--window", type=int, default=10, help="Rolling window size (default: 10)")
    parser.add_argument("--threshold", type=float, default=None,
                        help="Override regression threshold %% (default: per-metric)")
    parser.add_argument("--output", default=None, help="Output markdown file (default: stdout)")
    parser.add_argument("--output-json", default=None, help="Output comparison as JSON")
    args = parser.parse_args()

    # Load current results
    current_path = Path(args.current)
    if not current_path.exists():
        print(f"ERROR: current results file not found: {args.current}", file=sys.stderr)
        sys.exit(1)

    current = load_results_file(current_path)

    # Load baseline
    data_dir = Path(args.data_dir)
    baseline_branch = "main"  # Always compare against main
    historical = load_baseline_data(data_dir, baseline_branch, args.window)
    baseline = compute_baseline(historical)

    # Compare
    comparisons = compare_results(current, baseline, args.threshold)

    # Generate markdown
    markdown = generate_markdown(current, comparisons, args.window, len(historical))

    if args.output:
        Path(args.output).write_text(markdown)
        print(f"Markdown written to {args.output}")
    else:
        print(markdown)

    # Optionally output JSON comparison
    if args.output_json:
        comparison_data = {
            "commit": current.get("commit"),
            "timestamp": current.get("timestamp"),
            "has_regressions": any(c["status"] == "regression" for c in comparisons),
            "baseline_count": len(historical),
            "comparisons": comparisons,
        }
        Path(args.output_json).write_text(json.dumps(comparison_data, indent=2))

    # Always exit 0 (non-blocking)
    sys.exit(0)


if __name__ == "__main__":
    main()
