#!/usr/bin/env python3
"""
Analyze Simple N+X Top-N performance test results.

Reads JSON output from SimpleTopNTrackerPerfTest and generates:
- CPU breakdown (rebuild vs query vs callback latency)
- Memory growth across rounds
- Fast rejection effectiveness
- Latency percentile trends
- Marginal cost analysis (top-N overhead)

Usage:
    python3 tools/analyze_topn_perf.py perf_results.json [--html report.html]
"""

import argparse
import json
import sys
from pathlib import Path


def load_results(path: str) -> dict:
    with open(path) as f:
        return json.load(f)


def ns_to_us(ns: int) -> float:
    return ns / 1000.0


def format_bytes(b: int) -> str:
    if b >= 1024 * 1024:
        return f"{b / (1024*1024):.1f} MB"
    if b >= 1024:
        return f"{b / 1024:.1f} KB"
    return f"{b} B"


def print_summary(data: dict):
    config = data["config"]
    rounds = data["rounds"]

    print("\n" + "=" * 80)
    print("   SIMPLE N+X TOP-N PERFORMANCE ANALYSIS")
    print("=" * 80)

    print(f"\nTest Configuration:")
    print(f"  Panelists:      {config['panelists']}")
    print(f"  Subscribers:    {config['subscribers']}")
    print(f"  Top-N:          {config['top_n']}")
    print(f"  Rounds:         {config['rounds']}")
    print(f"  Round Duration: {config['round_duration_ms']} ms")
    print(f"  Group Interval: {config['group_interval_ms']} ms")
    print(f"  Reader Threads: {config['reader_threads']}")

    # --- CPU Breakdown ---
    print(f"\n{'─' * 80}")
    print("  CPU BREAKDOWN (average across rounds)")
    print(f"{'─' * 80}")

    avg_update_p50 = sum(r["update_latency"]["p50_ns"] for r in rounds) / len(rounds)
    avg_update_p99 = sum(r["update_latency"]["p99_ns"] for r in rounds) / len(rounds)
    avg_query_p50 = sum(r["query_latency"]["p50_ns"] for r in rounds) / len(rounds)
    avg_query_p99 = sum(r["query_latency"]["p99_ns"] for r in rounds) / len(rounds)
    avg_rebuild_p50 = sum(r["rebuild_latency"]["p50_ns"] for r in rounds) / len(rounds)
    avg_rebuild_p99 = sum(r["rebuild_latency"]["p99_ns"] for r in rounds) / len(rounds)
    avg_callback_p50 = sum(r["callback_latency"]["p50_ns"] for r in rounds) / len(rounds)
    avg_callback_p99 = sum(r["callback_latency"]["p99_ns"] for r in rounds) / len(rounds)

    print(f"\n  {'Operation':<20} {'p50 (us)':<12} {'p99 (us)':<12} {'Max (us)':<12}")
    print(f"  {'─' * 56}")
    max_update = max(r["update_latency"]["max_ns"] for r in rounds)
    max_query = max(r["query_latency"]["max_ns"] for r in rounds)
    max_rebuild = max(r["rebuild_latency"]["max_ns"] for r in rounds)
    max_callback = max(r["callback_latency"]["max_ns"] for r in rounds)

    print(f"  {'Update (full)':<20} {ns_to_us(avg_update_p50):<12.1f} {ns_to_us(avg_update_p99):<12.1f} {ns_to_us(max_update):<12.1f}")
    print(f"  {'Rebuild (sort)':<20} {ns_to_us(avg_rebuild_p50):<12.1f} {ns_to_us(avg_rebuild_p99):<12.1f} {ns_to_us(max_rebuild):<12.1f}")
    print(f"  {'Query (top-N)':<20} {ns_to_us(avg_query_p50):<12.1f} {ns_to_us(avg_query_p99):<12.1f} {ns_to_us(max_query):<12.1f}")
    print(f"  {'Callback (notify)':<20} {ns_to_us(avg_callback_p50):<12.1f} {ns_to_us(avg_callback_p99):<12.1f} {ns_to_us(max_callback):<12.1f}")

    # CPU time estimate
    total_updates = sum(r["throughput"]["updates"] for r in rounds)
    total_queries = sum(r["throughput"]["top_n_queries"] for r in rounds)
    total_duration = sum(r["duration_sec"] for r in rounds)

    update_cpu_ms = sum(r["update_latency"]["total_ns"] for r in rounds) / 1e6
    query_cpu_ms = sum(r["query_latency"]["total_ns"] for r in rounds) / 1e6
    callback_cpu_ms = sum(r["callback_latency"]["total_ns"] for r in rounds) / 1e6
    total_cpu_ms = update_cpu_ms + query_cpu_ms + callback_cpu_ms

    print(f"\n  CPU Time Breakdown (total wall-clock: {total_duration:.1f}s):")
    print(f"    Updates:    {update_cpu_ms:.1f} ms ({100*update_cpu_ms/total_cpu_ms:.1f}%)")
    print(f"    Queries:    {query_cpu_ms:.1f} ms ({100*query_cpu_ms/total_cpu_ms:.1f}%)")
    print(f"    Callbacks:  {callback_cpu_ms:.1f} ms ({100*callback_cpu_ms/total_cpu_ms:.1f}%)")
    print(f"    Total:      {total_cpu_ms:.1f} ms")

    cpu_utilization = total_cpu_ms / (total_duration * 1000) * 100
    print(f"    CPU util:   {cpu_utilization:.2f}% of one core")

    # --- Throughput ---
    print(f"\n{'─' * 80}")
    print("  THROUGHPUT")
    print(f"{'─' * 80}")

    avg_update_rate = total_updates / total_duration
    avg_query_rate = total_queries / total_duration
    print(f"\n  Avg Update Rate:    {avg_update_rate:.0f} updates/sec")
    print(f"  Avg Query Rate:     {avg_query_rate:.0f} queries/sec")
    print(f"  Total Updates:      {total_updates:,}")
    print(f"  Total Queries:      {total_queries:,}")

    # --- Fast Rejection ---
    print(f"\n{'─' * 80}")
    print("  FAST REJECTION OPTIMIZATION")
    print(f"{'─' * 80}")

    for r in rounds:
        fr = r["fast_rejection"]
        total = fr["fast_rejections"] + fr["full_scans"]
        rate = fr["rejection_rate_pct"]
        print(f"\n  Round {r['round']}: {rate:.1f}% rejected "
              f"({fr['fast_rejections']:,} fast / {fr['full_scans']:,} full scan)")

    avg_rejection = sum(r["fast_rejection"]["rejection_rate_pct"] for r in rounds) / len(rounds)
    print(f"\n  Average Rejection Rate: {avg_rejection:.1f}%")

    # --- Memory ---
    print(f"\n{'─' * 80}")
    print("  MEMORY USAGE")
    print(f"{'─' * 80}")

    print(f"\n  {'Round':<8} {'RSS Before':<14} {'RSS After':<14} {'Delta':<14} {'Snapshot':<12} {'Sessions':<12}")
    print(f"  {'─' * 72}")

    for r in rounds:
        m = r["memory"]
        delta = m["rss_after_bytes"] - m["rss_before_bytes"]
        print(f"  {r['round']:<8} {format_bytes(m['rss_before_bytes']):<14} "
              f"{format_bytes(m['rss_after_bytes']):<14} "
              f"{format_bytes(delta) if delta > 0 else '—':<14} "
              f"{format_bytes(m['snapshot_bytes']):<12} "
              f"{format_bytes(m['session_state_bytes']):<12}")

    last = rounds[-1]["memory"]
    print(f"\n  Snapshot overhead:  {format_bytes(last['snapshot_bytes'])} (shared across all subscribers)")
    print(f"  Session state:      {format_bytes(last['session_state_bytes'])} "
          f"({config['panelists'] + config['subscribers']} sessions)")
    total_topn_memory = last["snapshot_bytes"] + last["session_state_bytes"]
    print(f"  Total top-N memory: {format_bytes(total_topn_memory)}")

    # --- Speech Activity ---
    print(f"\n{'─' * 80}")
    print("  SPEECH ACTIVITY PATTERN")
    print(f"{'─' * 80}")

    for r in rounds:
        s = r["speech"]
        total_updates_r = s["speech_start_events"] + s["silent_updates"] + s["speaking_updates"]
        if total_updates_r > 0:
            print(f"\n  Round {r['round']}:")
            print(f"    Speech starts: {s['speech_start_events']:>6} "
                  f"({100*s['speech_start_events']/total_updates_r:.1f}%)")
            print(f"    Speaking:      {s['speaking_updates']:>6} "
                  f"({100*s['speaking_updates']/total_updates_r:.1f}%)")
            print(f"    Silent:        {s['silent_updates']:>6} "
                  f"({100*s['silent_updates']/total_updates_r:.1f}%)")

    # --- Correctness ---
    print(f"\n{'─' * 80}")
    print("  CORRECTNESS")
    print(f"{'─' * 80}")

    total_violations = sum(r["correctness"]["self_exclusion_violations"] for r in rounds)
    print(f"\n  Self-exclusion violations: {total_violations}")
    print(f"  Status: {'PASSED' if total_violations == 0 else 'FAILED'}")

    # --- Marginal Cost Analysis ---
    print(f"\n{'─' * 80}")
    print("  MARGINAL COST ANALYSIS")
    print(f"{'─' * 80}")

    # Per-object cost at egress
    per_query_ns = avg_query_p50
    per_update_ns = avg_update_p50

    # At 1M objects/sec with N subscribers per object
    objects_per_sec = 1_000_000
    subscriber_fan_out = config["subscribers"]
    queries_per_sec = objects_per_sec * subscriber_fan_out

    filter_cpu_per_sec = queries_per_sec * per_query_ns / 1e9
    update_cpu_per_sec = (config["panelists"] * 1000 / config["group_interval_ms"]) * per_update_ns / 1e9

    print(f"\n  Projected at 1M objects/sec, {subscriber_fan_out} subscribers:")
    print(f"    Filter evaluation: {filter_cpu_per_sec:.2f} CPU-seconds/sec ({filter_cpu_per_sec*100:.1f}% of one core)")
    print(f"    Updates:           {update_cpu_per_sec:.4f} CPU-seconds/sec")
    print(f"    Total top-N cost:  {(filter_cpu_per_sec + update_cpu_per_sec)*100:.1f}% of one core")

    if filter_cpu_per_sec < 1.0:
        print(f"\n  Verdict: Top-N filtering fits within a single core at this scale.")
    else:
        cores_needed = filter_cpu_per_sec + update_cpu_per_sec
        print(f"\n  Verdict: Top-N filtering requires ~{cores_needed:.1f} cores at this scale.")

    print(f"\n{'=' * 80}\n")


def generate_html_report(data: dict, output_path: str):
    """Generate an HTML report with embedded charts (no external dependencies)."""
    config = data["config"]
    rounds = data["rounds"]

    # Prepare chart data
    round_numbers = [r["round"] for r in rounds]
    update_p50 = [ns_to_us(r["update_latency"]["p50_ns"]) for r in rounds]
    update_p99 = [ns_to_us(r["update_latency"]["p99_ns"]) for r in rounds]
    query_p50 = [ns_to_us(r["query_latency"]["p50_ns"]) for r in rounds]
    query_p99 = [ns_to_us(r["query_latency"]["p99_ns"]) for r in rounds]
    rejection_rates = [r["fast_rejection"]["rejection_rate_pct"] for r in rounds]
    rss_mb = [r["memory"]["rss_after_bytes"] / (1024*1024) for r in rounds]

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Simple N+X Top-N Performance Report</title>
    <style>
        * {{ box-sizing: border-box; }}
        body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
               margin: 0; padding: 20px; background: #f8f9fa; color: #333; }}
        .container {{ max-width: 1200px; margin: 0 auto; }}
        h1 {{ color: #1a1a2e; margin-bottom: 5px; }}
        .subtitle {{ color: #666; margin-bottom: 30px; font-size: 14px; }}
        .grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(350px, 1fr)); gap: 20px; }}
        .card {{ background: white; border-radius: 12px; padding: 20px;
                 box-shadow: 0 2px 8px rgba(0,0,0,0.08); }}
        .card h3 {{ margin: 0 0 15px 0; color: #2d3436; font-size: 16px; }}
        .metric {{ display: flex; justify-content: space-between; padding: 8px 0;
                   border-bottom: 1px solid #f0f0f0; }}
        .metric:last-child {{ border-bottom: none; }}
        .metric-label {{ color: #636e72; font-size: 13px; }}
        .metric-value {{ font-weight: 600; color: #2d3436; }}
        .metric-value.good {{ color: #00b894; }}
        .metric-value.warn {{ color: #fdcb6e; }}
        .metric-value.bad {{ color: #d63031; }}
        .chart {{ width: 100%; height: 200px; position: relative; }}
        .bar-chart {{ display: flex; align-items: flex-end; gap: 4px; height: 160px; padding-top: 20px; }}
        .bar-group {{ flex: 1; display: flex; flex-direction: column; align-items: center; }}
        .bar {{ width: 100%; border-radius: 4px 4px 0 0; transition: height 0.3s; min-height: 2px; }}
        .bar.p50 {{ background: #0984e3; }}
        .bar.p99 {{ background: #d63031; opacity: 0.7; }}
        .bar-label {{ font-size: 11px; color: #999; margin-top: 5px; }}
        .legend {{ display: flex; gap: 15px; margin-bottom: 10px; font-size: 12px; }}
        .legend-item {{ display: flex; align-items: center; gap: 5px; }}
        .legend-dot {{ width: 10px; height: 10px; border-radius: 2px; }}
        table {{ width: 100%; border-collapse: collapse; font-size: 13px; }}
        th, td {{ padding: 8px 12px; text-align: right; border-bottom: 1px solid #eee; }}
        th {{ background: #f8f9fa; font-weight: 600; color: #636e72; }}
        td:first-child, th:first-child {{ text-align: left; }}
        .config-grid {{ display: grid; grid-template-columns: repeat(4, 1fr); gap: 10px; }}
        .config-item {{ text-align: center; padding: 12px; background: #f8f9fa; border-radius: 8px; }}
        .config-item .value {{ font-size: 24px; font-weight: 700; color: #0984e3; }}
        .config-item .label {{ font-size: 11px; color: #999; margin-top: 4px; }}
    </style>
</head>
<body>
<div class="container">
    <h1>Simple N+X Top-N Performance Report</h1>
    <p class="subtitle">Webinar simulation: {config['panelists']} panelists, {config['subscribers']} subscribers, top-{config['top_n']}, {config['rounds']} rounds x {config['round_duration_ms']/1000:.0f}s</p>

    <div class="config-grid" style="margin-bottom: 20px;">
        <div class="config-item"><div class="value">{config['panelists']}</div><div class="label">Panelists</div></div>
        <div class="config-item"><div class="value">{config['subscribers']}</div><div class="label">Subscribers</div></div>
        <div class="config-item"><div class="value">{config['top_n']}</div><div class="label">Top-N</div></div>
        <div class="config-item"><div class="value">{config['group_interval_ms']}ms</div><div class="label">Group Interval</div></div>
    </div>

    <div class="grid">
        <div class="card">
            <h3>Update Latency (per round)</h3>
            <div class="legend">
                <div class="legend-item"><div class="legend-dot" style="background:#0984e3"></div> p50</div>
                <div class="legend-item"><div class="legend-dot" style="background:#d63031"></div> p99</div>
            </div>
            <div class="bar-chart" id="update-chart"></div>
        </div>

        <div class="card">
            <h3>Query Latency (per round)</h3>
            <div class="legend">
                <div class="legend-item"><div class="legend-dot" style="background:#0984e3"></div> p50</div>
                <div class="legend-item"><div class="legend-dot" style="background:#d63031"></div> p99</div>
            </div>
            <div class="bar-chart" id="query-chart"></div>
        </div>

        <div class="card">
            <h3>Fast Rejection Rate</h3>
            <div class="bar-chart" id="rejection-chart"></div>
        </div>

        <div class="card">
            <h3>Memory (RSS after round)</h3>
            <div class="bar-chart" id="memory-chart"></div>
        </div>

        <div class="card" style="grid-column: span 2;">
            <h3>Per-Round Detail</h3>
            <table>
                <tr>
                    <th>Round</th>
                    <th>Updates</th>
                    <th>Queries</th>
                    <th>Update p50</th>
                    <th>Update p99</th>
                    <th>Query p50</th>
                    <th>Query p99</th>
                    <th>Rejection %</th>
                    <th>RSS</th>
                </tr>
"""

    for r in rounds:
        html += f"""                <tr>
                    <td>Round {r['round']}</td>
                    <td>{r['throughput']['updates']:,}</td>
                    <td>{r['throughput']['top_n_queries']:,}</td>
                    <td>{ns_to_us(r['update_latency']['p50_ns']):.1f} us</td>
                    <td>{ns_to_us(r['update_latency']['p99_ns']):.1f} us</td>
                    <td>{ns_to_us(r['query_latency']['p50_ns']):.1f} us</td>
                    <td>{ns_to_us(r['query_latency']['p99_ns']):.1f} us</td>
                    <td>{r['fast_rejection']['rejection_rate_pct']:.1f}%</td>
                    <td>{r['memory']['rss_after_bytes']/(1024*1024):.1f} MB</td>
                </tr>
"""

    html += f"""            </table>
        </div>
    </div>
</div>

<script>
const rounds = {json.dumps(round_numbers)};
const updateP50 = {json.dumps(update_p50)};
const updateP99 = {json.dumps(update_p99)};
const queryP50 = {json.dumps(query_p50)};
const queryP99 = {json.dumps(query_p99)};
const rejectionRates = {json.dumps(rejection_rates)};
const rssMb = {json.dumps(rss_mb)};

function renderBarChart(containerId, p50Data, p99Data, unit) {{
    const container = document.getElementById(containerId);
    const maxVal = Math.max(...p99Data, ...p50Data) * 1.1;
    container.innerHTML = '';
    for (let i = 0; i < rounds.length; i++) {{
        const group = document.createElement('div');
        group.className = 'bar-group';
        const h50 = (p50Data[i] / maxVal) * 140;
        const h99 = (p99Data[i] / maxVal) * 140;
        group.innerHTML = `
            <div style="display:flex;gap:2px;align-items:flex-end;width:100%;height:140px">
                <div class="bar p50" style="height:${{h50}}px;flex:1" title="p50: ${{p50Data[i].toFixed(1)}} ${{unit}}"></div>
                <div class="bar p99" style="height:${{h99}}px;flex:1" title="p99: ${{p99Data[i].toFixed(1)}} ${{unit}}"></div>
            </div>
            <div class="bar-label">R${{rounds[i]}}</div>`;
        container.appendChild(group);
    }}
}}

function renderSingleBarChart(containerId, values, unit, color) {{
    const container = document.getElementById(containerId);
    const maxVal = Math.max(...values) * 1.1;
    container.innerHTML = '';
    for (let i = 0; i < rounds.length; i++) {{
        const group = document.createElement('div');
        group.className = 'bar-group';
        const h = (values[i] / maxVal) * 140;
        group.innerHTML = `
            <div style="display:flex;align-items:flex-end;width:100%;height:140px">
                <div class="bar" style="height:${{h}}px;flex:1;background:${{color}}" title="${{values[i].toFixed(1)}} ${{unit}}"></div>
            </div>
            <div class="bar-label">R${{rounds[i]}}</div>`;
        container.appendChild(group);
    }}
}}

renderBarChart('update-chart', updateP50, updateP99, 'us');
renderBarChart('query-chart', queryP50, queryP99, 'us');
renderSingleBarChart('rejection-chart', rejectionRates, '%', '#00b894');
renderSingleBarChart('memory-chart', rssMb, 'MB', '#6c5ce7');
</script>
</body>
</html>"""

    with open(output_path, "w") as f:
        f.write(html)
    print(f"HTML report written to: {output_path}")


def main():
    parser = argparse.ArgumentParser(description="Analyze Simple N+X Top-N performance results")
    parser.add_argument("input", help="JSON results file from perf test")
    parser.add_argument("--html", help="Generate HTML report at this path")
    args = parser.parse_args()

    if not Path(args.input).exists():
        print(f"Error: {args.input} not found", file=sys.stderr)
        sys.exit(1)

    data = load_results(args.input)
    print_summary(data)

    if args.html:
        generate_html_report(data, args.html)


if __name__ == "__main__":
    main()
