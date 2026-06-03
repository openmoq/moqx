#!/usr/bin/env python3
"""
End-to-End Top-N Performance Test Orchestrator.

Starts a moqx relay, connects real QUIC/WebTransport clients with speech-pattern
property updates, collects relay metrics, and generates analysis + visualization.

Unlike the in-process SimpleTopNTrackerPerfTest, this exercises the FULL relay
stack: QUIC transport, session management, TopNFilter, object forwarding, and
subscriber fan-out.

Flow:
  1. Start moqx relay with generated config
  2. Run track_filter_load_test binary (panelists + subscribers with TRACK_FILTER)
  3. Poll relay /metrics endpoint during the test (CPU, memory, latency histograms)
  4. Collect results: relay logs, metrics time series, test report
  5. Generate analysis HTML and timeline visualization

Prerequisites:
  - moqx binary built: build/moqx
  - track_filter_load_test built: build/test/track_filter_load_test
  - Python 3.8+

Usage:
  python3 scripts/run_topn_e2e_perf.py [OPTIONS]

  Options:
    --build-dir PATH      Build directory (default: build/)
    --panelists N         Panelists/pub-subs (default: 50)
    --subscribers N       Pure subscribers (default: 500)
    --top-n N             Top-N value (default: 5)
    --duration N          Test duration in seconds (default: 30)
    --update-hz N         Property update rate (default: 50)
    --rounds N            Number of test rounds (default: 3)
    --relay-port N        Relay listen port (default: 4433)
    --admin-port N        Admin HTTP port (default: 19701)
    --io-threads N        Relay IO threads (default: 1)
    --output-dir PATH     Output directory (default: perf_results/)
    --relay-log SPEC      Relay log level (default: warn)
    --no-open             Don't open HTML in browser
    --keep-relay          Don't stop relay after test
"""

import argparse
import json
import os
import platform
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request
from datetime import datetime
from pathlib import Path
from typing import Optional


def find_project_root() -> Path:
    """Find the moqx project root."""
    script_dir = Path(__file__).resolve().parent
    return script_dir.parent


def check_binary(path: Path, name: str):
    if not path.exists():
        print(f"Error: {name} not found at {path}", file=sys.stderr)
        print(f"  Build it with: ninja -C build {path.name}", file=sys.stderr)
        sys.exit(1)
    if not os.access(path, os.X_OK):
        print(f"Error: {name} not executable: {path}", file=sys.stderr)
        sys.exit(1)


def check_port_available(port: int) -> bool:
    import socket
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        return s.connect_ex(("127.0.0.1", port)) != 0


def wait_for_relay(admin_port: int, timeout: float = 15.0) -> bool:
    """Wait for relay admin endpoint to become ready."""
    start = time.time()
    url = f"http://127.0.0.1:{admin_port}/info"
    while time.time() - start < timeout:
        try:
            resp = urllib.request.urlopen(url, timeout=2)
            if resp.status == 200:
                return True
        except Exception:
            pass
        time.sleep(0.2)
    return False


def generate_relay_config(
    relay_port: int,
    admin_port: int,
    io_threads: int,
    config_path: Path,
):
    """Generate relay YAML config for the test."""
    config = f"""relay_id: "topn-e2e-perf-test"
threads: {io_threads}
use_relay_thread: true
listeners:
  - name: perf
    udp:
      socket:
        address: "::"
        port: {relay_port}
    tls:
      insecure: true
    endpoint: "/moq-relay"
    mvfst:
      enable_gso: true
    quic:
      cc_algo: bbr2
services:
  default:
    match:
      - authority: {{any: true}}
        path: {{prefix: "/"}}
    cache:
      enabled: false
      max_tracks: 0
      max_groups_per_track: 0
    ranking_mode: simple
admin:
  port: {admin_port}
  address: "::"
  plaintext: true
"""
    config_path.write_text(config)
    return config_path


def poll_metrics(admin_port: int) -> Optional[dict]:
    """Poll relay /metrics endpoint and parse Prometheus format."""
    url = f"http://127.0.0.1:{admin_port}/metrics"
    try:
        resp = urllib.request.urlopen(url, timeout=2)
        text = resp.read().decode()
        metrics = {}
        for line in text.split("\n"):
            if line.startswith("#") or not line.strip():
                continue
            parts = line.split(" ", 1)
            if len(parts) == 2:
                try:
                    metrics[parts[0]] = float(parts[1])
                except ValueError:
                    pass
        return metrics
    except Exception:
        return None


def poll_state(admin_port: int) -> Optional[dict]:
    """Poll relay /state endpoint for full state dump."""
    url = f"http://127.0.0.1:{admin_port}/state"
    try:
        resp = urllib.request.urlopen(url, timeout=2)
        return json.loads(resp.read().decode())
    except Exception:
        return None


class MetricsCollector:
    """Collects time-series metrics from relay admin endpoint."""

    def __init__(self, admin_port: int, interval_sec: float = 1.0):
        self.admin_port = admin_port
        self.interval = interval_sec
        self.samples = []
        self._running = False
        self._thread = None

    def start(self):
        import threading
        self._running = True
        self._thread = threading.Thread(target=self._poll_loop, daemon=True)
        self._thread.start()

    def stop(self):
        self._running = False
        if self._thread:
            self._thread.join(timeout=5)

    def _poll_loop(self):
        while self._running:
            metrics = poll_metrics(self.admin_port)
            if metrics:
                metrics["_timestamp"] = time.time()
                self.samples.append(metrics)
            time.sleep(self.interval)

    def summary(self) -> dict:
        if not self.samples:
            return {}

        # Extract key metrics time series
        result = {
            "sample_count": len(self.samples),
            "duration_sec": self.samples[-1]["_timestamp"] - self.samples[0]["_timestamp"]
                if len(self.samples) > 1 else 0,
        }

        # Peak values
        sessions_key = "moqx_moqActiveSessions"
        if any(sessions_key in s for s in self.samples):
            result["peak_active_sessions"] = max(
                s.get(sessions_key, 0) for s in self.samples
            )

        # Latency histograms (if available)
        latency_sum = "moqx_moqObjectProcessingLatency_milliseconds_sum"
        latency_count = "moqx_moqObjectProcessingLatency_milliseconds_count"
        if self.samples[-1].get(latency_count, 0) > 0:
            total_latency = self.samples[-1].get(latency_sum, 0)
            total_count = self.samples[-1].get(latency_count, 0)
            result["avg_object_latency_ms"] = total_latency / total_count if total_count else 0
            result["total_objects_processed"] = int(total_count)

        return result


def run_round(
    args,
    round_num: int,
    relay_process,
    project_root: Path,
    output_dir: Path,
) -> dict:
    """Run a single test round using track_filter_load_test."""

    test_binary = project_root / args.build_dir / "test" / "track_filter_load_test"
    relay_url = f"https://127.0.0.1:{args.relay_port}/moq-relay"
    report_file = output_dir / f"round_{round_num}_report.txt"

    print(f"\n  Starting round {round_num}...")

    # Start metrics collection
    collector = MetricsCollector(args.admin_port, interval_sec=1.0)
    collector.start()

    round_start = time.time()

    # Run the load test
    cmd = [
        str(test_binary),
        f"--relay_url={relay_url}",
        f"--panelists={args.panelists}",
        f"--subscribers={args.subscribers}",
        f"--top_n={args.top_n}",
        f"--duration={args.duration}",
        f"--update_hz={args.update_hz}",
        f"--insecure",
        f"--report_file={report_file}",
        f"--batch_size=50",
        f"--batch_delay_ms=50",
        f"--drain_period_ms=3000",
    ]

    print(f"  Command: {' '.join(cmd[:4])}...")

    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=args.duration * 3 + 60,  # generous timeout
    )

    round_duration = time.time() - round_start
    collector.stop()

    # Save stdout/stderr
    stdout_path = output_dir / f"round_{round_num}_stdout.txt"
    stderr_path = output_dir / f"round_{round_num}_stderr.txt"
    stdout_path.write_text(proc.stdout)
    if proc.stderr:
        stderr_path.write_text(proc.stderr)

    # Parse report for key metrics
    round_metrics = {
        "round": round_num,
        "exit_code": proc.returncode,
        "duration_sec": round_duration,
        "relay_metrics": collector.summary(),
    }

    # Parse the text report for structured data
    if report_file.exists():
        report_text = report_file.read_text()
        round_metrics["report"] = parse_report(report_text)
    else:
        round_metrics["report"] = {}
        print(f"  WARNING: No report generated for round {round_num}")

    if proc.returncode != 0:
        print(f"  WARNING: Round {round_num} exited with code {proc.returncode}")
        if proc.stderr:
            # Print last few lines of stderr
            lines = proc.stderr.strip().split("\n")
            for line in lines[-5:]:
                print(f"    {line}")

    # Poll final relay state
    state = poll_state(args.admin_port)
    if state:
        round_metrics["relay_state"] = {
            "subscriptions": len(state.get("subscriptions", [])),
            "namespace_tree_depth": len(state.get("namespace_tree", [])),
        }

    return round_metrics


def parse_report(text: str) -> dict:
    """Parse the track_filter_load_test report text into structured data."""
    result = {}

    lines = text.split("\n")
    for line in lines:
        line = line.strip()
        if ":" not in line:
            continue

        # Extract key metrics
        if "Panelists Connected:" in line:
            parts = line.split(":")[-1].strip().split("/")
            result["panelists_connected"] = int(parts[0].strip())
        elif "Subscribers Connected:" in line:
            parts = line.split(":")[-1].strip().split("/")
            result["subscribers_connected"] = int(parts[0].strip())
        elif "Connection Errors:" in line:
            result["connection_errors"] = int(line.split(":")[-1].strip())
        elif "Objects Published:" in line:
            result["objects_published"] = int(line.split(":")[-1].strip())
        elif "Objects Received:" in line:
            result["objects_received"] = int(line.split(":")[-1].strip())
        elif "Self-Received (errors):" in line:
            result["self_received"] = int(line.split(":")[-1].strip())
        elif "Subscriber Failures:" in line:
            val = line.split(":")[-1].strip()
            result["subscriber_failures"] = int(val) if val.isdigit() else 0
        elif "Panelist Failures:" in line:
            val = line.split(":")[-1].strip()
            result["panelist_failures"] = int(val) if val.isdigit() else 0
        elif "Publish Rate:" in line and "obj/s" in line:
            val = line.split(":")[-1].strip().split(" ")[0]
            try:
                result["publish_rate"] = float(val)
            except ValueError:
                pass
        elif "Receive Rate:" in line and "obj/s" in line:
            val = line.split(":")[-1].strip().split(" ")[0]
            try:
                result["receive_rate"] = float(val)
            except ValueError:
                pass
        elif "Overall Status:" in line:
            result["status"] = "PASSED" if "PASSED" in line else "FAILED"

    return result


def generate_summary_report(all_rounds: list, args, output_dir: Path) -> dict:
    """Generate aggregate summary from all rounds."""
    summary = {
        "test": "TopN_E2E_PerfTest",
        "timestamp": datetime.now().isoformat(),
        "config": {
            "panelists": args.panelists,
            "subscribers": args.subscribers,
            "top_n": args.top_n,
            "duration_per_round": args.duration,
            "update_hz": args.update_hz,
            "rounds": args.rounds,
            "io_threads": args.io_threads,
            "relay_port": args.relay_port,
        },
        "rounds": all_rounds,
    }

    # Aggregates
    successful_rounds = [r for r in all_rounds if r.get("exit_code") == 0]
    if successful_rounds:
        reports = [r.get("report", {}) for r in successful_rounds]

        avg_publish_rate = sum(r.get("publish_rate", 0) for r in reports) / len(reports)
        avg_receive_rate = sum(r.get("receive_rate", 0) for r in reports) / len(reports)
        total_published = sum(r.get("objects_published", 0) for r in reports)
        total_received = sum(r.get("objects_received", 0) for r in reports)
        total_self = sum(r.get("self_received", 0) for r in reports)
        total_sub_failures = sum(r.get("subscriber_failures", 0) for r in reports)
        total_panelist_failures = sum(r.get("panelist_failures", 0) for r in reports)

        summary["aggregate"] = {
            "successful_rounds": len(successful_rounds),
            "avg_publish_rate_per_sec": avg_publish_rate,
            "avg_receive_rate_per_sec": avg_receive_rate,
            "total_objects_published": total_published,
            "total_objects_received": total_received,
            "total_self_exclusion_violations": total_self,
            "total_subscriber_failures": total_sub_failures,
            "total_panelist_failures": total_panelist_failures,
            "overall_status": "PASSED" if (total_self == 0 and total_sub_failures == 0 and total_panelist_failures == 0) else "FAILED",
        }

        # Relay metrics aggregate
        relay_metrics = [r.get("relay_metrics", {}) for r in successful_rounds if r.get("relay_metrics")]
        if relay_metrics:
            peak_sessions = max(m.get("peak_active_sessions", 0) for m in relay_metrics)
            summary["aggregate"]["peak_relay_sessions"] = peak_sessions

    return summary


def generate_e2e_html_report(summary: dict, output_path: Path):
    """Generate HTML report for e2e results."""
    config = summary["config"]
    rounds = summary["rounds"]
    agg = summary.get("aggregate", {})

    rounds_json = json.dumps([{
        "round": r["round"],
        "duration": r["duration_sec"],
        "published": r.get("report", {}).get("objects_published", 0),
        "received": r.get("report", {}).get("objects_received", 0),
        "pub_rate": r.get("report", {}).get("publish_rate", 0),
        "recv_rate": r.get("report", {}).get("receive_rate", 0),
        "status": r.get("report", {}).get("status", "UNKNOWN"),
        "peak_sessions": r.get("relay_metrics", {}).get("peak_active_sessions", 0),
    } for r in rounds])

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Top-N E2E Performance Report</title>
    <style>
        * {{ box-sizing: border-box; }}
        body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
               margin: 0; padding: 20px; background: #f8f9fa; color: #333; }}
        .container {{ max-width: 1200px; margin: 0 auto; }}
        h1 {{ color: #1a1a2e; margin-bottom: 5px; }}
        .subtitle {{ color: #666; margin-bottom: 30px; font-size: 14px; }}
        .grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 16px; margin-bottom: 20px; }}
        .card {{ background: white; border-radius: 10px; padding: 20px;
                 box-shadow: 0 2px 6px rgba(0,0,0,0.06); text-align: center; }}
        .card .value {{ font-size: 28px; font-weight: 700; color: #0984e3; }}
        .card .label {{ font-size: 12px; color: #636e72; margin-top: 4px; }}
        .card.pass .value {{ color: #00b894; }}
        .card.fail .value {{ color: #d63031; }}
        table {{ width: 100%; border-collapse: collapse; background: white;
                 border-radius: 10px; overflow: hidden; box-shadow: 0 2px 6px rgba(0,0,0,0.06); }}
        th, td {{ padding: 12px 16px; text-align: right; border-bottom: 1px solid #f0f0f0; }}
        th {{ background: #f8f9fa; font-weight: 600; color: #636e72; font-size: 13px; }}
        td:first-child, th:first-child {{ text-align: left; }}
        .status-pass {{ color: #00b894; font-weight: 600; }}
        .status-fail {{ color: #d63031; font-weight: 600; }}
    </style>
</head>
<body>
<div class="container">
    <h1>Top-N E2E Performance Report</h1>
    <p class="subtitle">
        {config['panelists']} panelists, {config['subscribers']} subscribers,
        top-{config['top_n']}, {config['update_hz']} Hz updates,
        {config['rounds']} rounds x {config['duration_per_round']}s
    </p>

    <div class="grid">
        <div class="card {'pass' if agg.get('overall_status') == 'PASSED' else 'fail'}">
            <div class="value">{agg.get('overall_status', 'N/A')}</div>
            <div class="label">Overall Status</div>
        </div>
        <div class="card">
            <div class="value">{agg.get('avg_publish_rate_per_sec', 0):.0f}</div>
            <div class="label">Avg Publish Rate (obj/s)</div>
        </div>
        <div class="card">
            <div class="value">{agg.get('avg_receive_rate_per_sec', 0):.0f}</div>
            <div class="label">Avg Receive Rate (obj/s)</div>
        </div>
        <div class="card">
            <div class="value">{agg.get('peak_relay_sessions', 0)}</div>
            <div class="label">Peak Relay Sessions</div>
        </div>
        <div class="card {'pass' if agg.get('total_self_exclusion_violations', 0) == 0 else 'fail'}">
            <div class="value">{agg.get('total_self_exclusion_violations', 0)}</div>
            <div class="label">Self-Exclusion Violations</div>
        </div>
        <div class="card">
            <div class="value">{agg.get('successful_rounds', 0)}/{config['rounds']}</div>
            <div class="label">Successful Rounds</div>
        </div>
    </div>

    <table>
        <tr>
            <th>Round</th>
            <th>Duration (s)</th>
            <th>Published</th>
            <th>Received</th>
            <th>Pub Rate</th>
            <th>Recv Rate</th>
            <th>Peak Sessions</th>
            <th>Status</th>
        </tr>
        <tbody id="rounds-table"></tbody>
    </table>
</div>

<script>
const rounds = {rounds_json};
const tbody = document.getElementById('rounds-table');
rounds.forEach(r => {{
    const statusClass = r.status === 'PASSED' ? 'status-pass' : 'status-fail';
    tbody.innerHTML += `<tr>
        <td>Round ${{r.round}}</td>
        <td>${{r.duration.toFixed(1)}}</td>
        <td>${{r.published.toLocaleString()}}</td>
        <td>${{r.received.toLocaleString()}}</td>
        <td>${{r.pub_rate.toFixed(0)}} obj/s</td>
        <td>${{r.recv_rate.toFixed(0)}} obj/s</td>
        <td>${{r.peak_sessions}}</td>
        <td class="${{statusClass}}">${{r.status}}</td>
    </tr>`;
}});
</script>
</body>
</html>"""

    output_path.write_text(html)


def main():
    parser = argparse.ArgumentParser(description="End-to-end Top-N performance test orchestrator")
    parser.add_argument("--build-dir", default="build", help="Build directory")
    parser.add_argument("--panelists", type=int, default=50)
    parser.add_argument("--subscribers", type=int, default=500)
    parser.add_argument("--top-n", type=int, default=5)
    parser.add_argument("--duration", type=int, default=30, help="Duration per round (seconds)")
    parser.add_argument("--update-hz", type=int, default=50, help="Property update rate")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--relay-port", type=int, default=4433)
    parser.add_argument("--admin-port", type=int, default=19701)
    parser.add_argument("--io-threads", type=int, default=1)
    parser.add_argument("--output-dir", default="perf_results")
    parser.add_argument("--relay-log", default="warn")
    parser.add_argument("--no-open", action="store_true")
    parser.add_argument("--keep-relay", action="store_true")
    args = parser.parse_args()

    project_root = find_project_root()

    # Check binaries exist
    relay_binary = project_root / args.build_dir / "moqx"
    test_binary = project_root / args.build_dir / "test" / "track_filter_load_test"
    check_binary(relay_binary, "moqx relay")
    check_binary(test_binary, "track_filter_load_test")

    # Check ports
    if not check_port_available(args.relay_port):
        print(f"Error: port {args.relay_port} already in use", file=sys.stderr)
        sys.exit(1)
    if not check_port_available(args.admin_port):
        print(f"Error: port {args.admin_port} already in use", file=sys.stderr)
        sys.exit(1)

    # Setup output
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    output_dir = Path(args.output_dir) / f"e2e-{timestamp}"
    output_dir.mkdir(parents=True, exist_ok=True)

    print("=" * 80)
    print("  moqx Top-N E2E Performance Test")
    print("=" * 80)
    print(f"\n  Panelists:    {args.panelists}")
    print(f"  Subscribers:  {args.subscribers}")
    print(f"  Top-N:        {args.top_n}")
    print(f"  Update Hz:    {args.update_hz}")
    print(f"  Rounds:       {args.rounds} x {args.duration}s")
    print(f"  IO Threads:   {args.io_threads}")
    print(f"  Output:       {output_dir}")
    print()

    # Generate config
    config_path = output_dir / "relay.yaml"
    generate_relay_config(args.relay_port, args.admin_port, args.io_threads, config_path)

    # Start relay
    print("Step 1: Starting relay...")
    relay_log_path = output_dir / "relay.log"
    relay_log = open(relay_log_path, "w")

    relay_cmd = [
        str(relay_binary),
        f"--config={config_path}",
        f"--logging={args.relay_log}",
    ]
    relay_proc = subprocess.Popen(
        relay_cmd,
        stdout=relay_log,
        stderr=subprocess.STDOUT,
    )

    try:
        if not wait_for_relay(args.admin_port):
            print("Error: Relay failed to start within 15 seconds", file=sys.stderr)
            relay_proc.terminate()
            sys.exit(1)
        print(f"  Relay ready (PID {relay_proc.pid}, port {args.relay_port})")

        # Run rounds
        print(f"\nStep 2: Running {args.rounds} test rounds...")
        all_rounds = []
        for round_num in range(1, args.rounds + 1):
            try:
                result = run_round(args, round_num, relay_proc, project_root, output_dir)
                all_rounds.append(result)

                status = result.get("report", {}).get("status", "UNKNOWN")
                print(f"  Round {round_num}: {status} "
                      f"(pub: {result.get('report', {}).get('objects_published', 0):,}, "
                      f"recv: {result.get('report', {}).get('objects_received', 0):,})")

            except subprocess.TimeoutExpired:
                print(f"  Round {round_num}: TIMEOUT", file=sys.stderr)
                all_rounds.append({"round": round_num, "exit_code": -1, "report": {"status": "TIMEOUT"}})
            except Exception as e:
                print(f"  Round {round_num}: ERROR - {e}", file=sys.stderr)
                all_rounds.append({"round": round_num, "exit_code": -1, "report": {"status": "ERROR"}})

            # Brief pause between rounds for relay cleanup
            if round_num < args.rounds:
                time.sleep(2)

        # Generate summary
        print("\nStep 3: Generating reports...")
        summary = generate_summary_report(all_rounds, args, output_dir)

        # Write JSON
        json_path = output_dir / "summary.json"
        json_path.write_text(json.dumps(summary, indent=2))

        # Write HTML
        html_path = output_dir / "report.html"
        generate_e2e_html_report(summary, html_path)

        # Print summary
        agg = summary.get("aggregate", {})
        print(f"\n{'=' * 80}")
        print("  RESULTS")
        print(f"{'=' * 80}")
        print(f"\n  Status:           {agg.get('overall_status', 'N/A')}")
        print(f"  Avg Publish Rate: {agg.get('avg_publish_rate_per_sec', 0):.0f} obj/s")
        print(f"  Avg Receive Rate: {agg.get('avg_receive_rate_per_sec', 0):.0f} obj/s")
        print(f"  Peak Sessions:    {agg.get('peak_relay_sessions', 0)}")
        print(f"  Self Violations:  {agg.get('total_self_exclusion_violations', 0)}")
        print(f"\n  JSON:   {json_path}")
        print(f"  HTML:   {html_path}")
        print(f"  Logs:   {output_dir}")
        print()

        if not args.no_open:
            if platform.system() == "Darwin":
                subprocess.run(["open", str(html_path)], check=False)
            elif platform.system() == "Linux":
                subprocess.run(["xdg-open", str(html_path)], check=False)

    finally:
        if not args.keep_relay:
            print("Stopping relay...")
            relay_proc.terminate()
            try:
                relay_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                relay_proc.kill()
        else:
            print(f"Relay still running (PID {relay_proc.pid})")

        relay_log.close()

    print("Done.")


if __name__ == "__main__":
    main()
