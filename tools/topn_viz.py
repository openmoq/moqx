#!/usr/bin/env python3
"""
Top-N Filtering Interactive Timeline Visualization.

Reads TOPN_EVENT structured logs (same format as moq-rs) and generates a
self-contained interactive HTML file showing:
- Publisher speech activity timeline (per-publisher lanes)
- Subscriber grid showing who sees which publisher at any time
- Time scrubber for navigating the timeline
- Self-exclusion visualization

Input format (one per line):
    TOPN_EVENT:{"event":"track_registered","ts_ms":100,"track":"panelist-0","value":0,"publisher_id":0}
    TOPN_EVENT:{"event":"value_updated","ts_ms":200,"track":"panelist-0","old_value":0,"new_value":2,"publisher_id":0}
    TOPN_EVENT:{"event":"subscriber_registered","ts_ms":50,"subscriber_id":0,"is_pub_sub":true,"publisher_id":0}
    TOPN_EVENT:{"event":"top_n_query","ts_ms":300,"subscriber_id":1,"n":5,"selected":[...],"excluded_self":0}

Usage:
    python3 tools/topn_viz.py topn_events.log -o topn_timeline.html
    python3 tools/topn_viz.py topn_events.log --open  # Generate and open in browser
"""

import argparse
import json
import os
import platform
import subprocess
import sys
from pathlib import Path


def parse_events(input_path: str) -> list:
    events = []
    with open(input_path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("TOPN_EVENT:"):
                json_str = line[len("TOPN_EVENT:"):]
                try:
                    events.append(json.loads(json_str))
                except json.JSONDecodeError as e:
                    print(f"Warning: failed to parse: {json_str[:80]}... ({e})", file=sys.stderr)
    return events


def process_events(events: list) -> dict:
    publisher_tracks = {}  # track_name -> (publisher_id, [(ts, value)])
    subscribers = {}       # sub_id -> {id, is_publisher, publisher_id}
    queries = []
    track_to_publisher = {}
    top_n = 5
    min_ts = float("inf")
    max_ts = 0

    # Map raw session pointers to small sequential IDs for display
    raw_to_display = {}
    next_pub_id = [0]
    next_sub_id = [0]

    def display_pub_id(raw_id):
        if raw_id not in raw_to_display:
            raw_to_display[raw_id] = next_pub_id[0]
            next_pub_id[0] += 1
        return raw_to_display[raw_id]

    def display_sub_id(raw_id):
        if raw_id not in raw_to_display:
            raw_to_display[raw_id] = next_sub_id[0]
            next_sub_id[0] += 1
        return raw_to_display[raw_id]

    for ev in events:
        ts = ev.get("ts_ms", 0)
        min_ts = min(min_ts, ts)
        max_ts = max(max_ts, ts)

        event_type = ev.get("event")

        if event_type == "track_registered":
            track = ev["track"]
            raw_pub = ev["publisher_id"]
            pub_id = display_pub_id(raw_pub)
            value = ev.get("value", ev.get("initial_value", 0))
            track_to_publisher[track] = pub_id
            if track not in publisher_tracks:
                publisher_tracks[track] = (pub_id, [])
            publisher_tracks[track][1].append((ts, value))

        elif event_type == "value_updated":
            track = ev["track"]
            raw_pub = ev["publisher_id"]
            pub_id = display_pub_id(raw_pub)
            new_value = ev["new_value"]
            track_to_publisher[track] = pub_id
            if track not in publisher_tracks:
                publisher_tracks[track] = (pub_id, [])
            publisher_tracks[track][1].append((ts, new_value))

        elif event_type == "subscriber_registered":
            raw_sub = ev["subscriber_id"]
            sub_id = display_sub_id(raw_sub)
            # Detect pub-sub: if this subscriber_id was also seen as publisher_id
            is_pub_sub = raw_sub in raw_to_display and ev.get("is_pub_sub", False)
            subscribers[sub_id] = {
                "id": sub_id,
                "is_publisher": is_pub_sub or ev.get("is_pub_sub", False),
                "publisher_id": raw_to_display.get(raw_sub),
            }

        elif event_type == "top_n_selected":
            track = ev.get("track", "")
            raw_sub = ev["subscriber_id"]
            sub_id = display_sub_id(raw_sub)
            pub_id = track_to_publisher.get(track)
            if pub_id is not None:
                queries.append({
                    "ts_ms": ts,
                    "subscriber_id": sub_id,
                    "selected": [pub_id],
                    "action": "selected",
                    "track": track,
                })

        elif event_type == "top_n_evicted":
            track = ev.get("track", "")
            raw_sub = ev["subscriber_id"]
            sub_id = display_sub_id(raw_sub)
            pub_id = track_to_publisher.get(track)
            if pub_id is not None:
                queries.append({
                    "ts_ms": ts,
                    "subscriber_id": sub_id,
                    "selected": [pub_id],
                    "action": "evicted",
                    "track": track,
                })

        elif event_type == "top_n_query":
            top_n = ev.get("n", top_n)
            raw_sub = ev["subscriber_id"]
            sub_id = display_sub_id(raw_sub)
            selected_ids = []
            for s in ev.get("selected", []):
                track = s.get("track", "")
                if track in track_to_publisher:
                    selected_ids.append(track_to_publisher[track])
            queries.append({
                "ts_ms": ts,
                "subscriber_id": sub_id,
                "selected": selected_ids,
                "excluded_self": ev.get("excluded_self"),
            })

        elif event_type == "track_removed":
            pass  # Timeline ends at last value_updated

    if min_ts == float("inf"):
        min_ts = 0
    duration = max(max_ts - min_ts, 1000)

    # Build publisher timelines
    publishers = []
    for track, (pub_id, changes) in publisher_tracks.items():
        changes.sort(key=lambda x: x[0])
        timeline = []
        for i, (ts, value) in enumerate(changes):
            end_ts = changes[i + 1][0] if i + 1 < len(changes) else max_ts
            timeline.append({
                "start": ts - min_ts,
                "end": end_ts - min_ts,
                "value": value,
            })
        publishers.append({"id": pub_id, "track": track, "timeline": timeline})

    publishers.sort(key=lambda p: p["id"])

    # Deduplicate publishers by ID (keep first track)
    seen_ids = set()
    deduped = []
    for p in publishers:
        if p["id"] not in seen_ids:
            seen_ids.add(p["id"])
            deduped.append(p)
    publishers = deduped

    # Build subscriber list
    subs = sorted(subscribers.values(), key=lambda s: s["id"])

    # Adjust query timestamps and separate selection events
    adjusted_queries = []
    selection_events = []
    for q in queries:
        q_copy = dict(q)
        q_copy["ts_ms"] = q["ts_ms"] - min_ts
        if "action" in q_copy:
            selection_events.append(q_copy)
        else:
            adjusted_queries.append(q_copy)

    # Detect pub-sub relationships: if a subscriber_id was also registered as
    # a publisher (track_registered came first), mark them
    for sub_id, sub in subscribers.items():
        if not sub.get("is_publisher"):
            # Check if this sub's raw ID matches any publisher's raw ID
            # by seeing if they share an ID in the display map
            if sub_id < len(publishers):
                sub["is_publisher"] = True
                sub["publisher_id"] = sub_id

    return {
        "duration": duration,
        "top_n": top_n,
        "publishers": publishers,
        "subscribers": subs,
        "queries": adjusted_queries,
        "selection_events": selection_events,
    }


def generate_html(viz_data: dict) -> str:
    data_json = json.dumps(viz_data)

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>moqx Top-N Filtering Interactive Timeline</title>
    <style>
        * {{ box-sizing: border-box; }}
        body {{
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            margin: 0; padding: 20px; background: #f5f5f5;
        }}
        .container {{ max-width: 1400px; margin: 0 auto; }}
        h1 {{ color: #333; margin-bottom: 5px; }}
        .subtitle {{ color: #666; margin-bottom: 20px; }}

        .controls {{
            background: white; padding: 15px; border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1); margin-bottom: 20px;
            display: flex; flex-wrap: wrap; align-items: center; gap: 10px;
        }}
        .controls label {{ font-weight: 600; }}
        .pub-btn {{
            padding: 8px 14px; margin: 2px; border: 2px solid #ddd;
            background: white; border-radius: 6px; cursor: pointer;
            font-size: 13px; transition: all 0.2s;
        }}
        .pub-btn:hover {{ border-color: #2196F3; }}
        .pub-btn.active {{ background: #2196F3; color: white; border-color: #2196F3; }}

        .timeline-container {{
            background: white; padding: 20px; border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }}

        .timeline-header {{
            display: flex; align-items: center; margin-bottom: 10px; flex-wrap: wrap; gap: 10px;
        }}
        .timeline-header h3 {{ margin: 0; margin-right: 20px; }}
        .legend {{ display: flex; gap: 15px; font-size: 12px; flex-wrap: wrap; }}
        .legend-item {{ display: flex; align-items: center; gap: 5px; }}
        .legend-box {{ width: 16px; height: 16px; border-radius: 3px; }}

        .stacked-timelines {{ margin-top: 10px; }}
        .stacked-row {{ display: flex; align-items: center; margin-bottom: 4px; }}
        .stacked-row.selected {{ margin-bottom: 8px; }}
        .stacked-label {{ width: 80px; font-size: 12px; color: #666; font-weight: 600; flex-shrink: 0;
                          white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }}
        .stacked-label.selected {{ color: #2196F3; font-size: 13px; font-weight: 700; }}

        .timeline-track {{
            height: 28px; display: flex; border-radius: 4px; overflow: hidden;
            border: 1px solid #ddd; position: relative; flex: 1; cursor: crosshair;
        }}
        .timeline-track.selected {{ height: 44px; border-width: 2px; border-color: #2196F3; }}
        .time-block {{
            display: flex; align-items: center; justify-content: center;
            font-weight: 600; font-size: 12px; min-width: 1px; position: relative;
        }}
        .time-block.silent {{ background: #bdbdbd; }}
        .time-block.speaking {{ background: #4caf50; }}
        .time-block.start {{ background: #ff9800; }}
        .time-block .value-label {{
            position: absolute; font-size: 10px; color: white;
            background: rgba(0,0,0,0.3); padding: 1px 4px; border-radius: 2px;
        }}

        .scrubber {{
            position: absolute; top: 0; bottom: 0; width: 2px;
            background: #f44336; pointer-events: none; z-index: 10;
        }}
        .scrubber::after {{
            content: ''; position: absolute; top: -4px; left: -3px;
            width: 8px; height: 8px; background: #f44336; border-radius: 50%;
        }}

        .time-display {{
            font-size: 14px; color: #333; font-weight: 600;
            background: #fff; padding: 5px 10px; border-radius: 4px;
            border: 1px solid #ddd;
        }}

        .block-detail {{
            margin-top: 20px; padding: 15px; background: #f8f9fa;
            border-radius: 6px; border-left: 4px solid #2196F3;
        }}
        .block-detail h4 {{ margin: 0 0 10px 0; }}
        .stats {{ display: flex; gap: 30px; margin-bottom: 15px; flex-wrap: wrap; }}
        .stat {{ text-align: center; min-width: 80px; }}
        .stat-value {{ font-size: 24px; font-weight: 700; color: #2196F3; }}
        .stat-label {{ font-size: 12px; color: #666; }}

        .subscriber-section {{ margin-top: 20px; }}
        .subscriber-section h4 {{ margin: 0 0 10px 0; color: #555; }}
        .subscriber-grid {{
            display: grid; grid-template-columns: repeat(auto-fill, minmax(130px, 1fr)); gap: 8px;
        }}
        .sub-card {{
            padding: 8px; border-radius: 6px; text-align: center;
            font-size: 12px; transition: all 0.3s;
        }}
        .sub-card.sees {{ background: #e3f2fd; border: 2px solid #2196F3; }}
        .sub-card.not-sees {{ background: #fafafa; border: 2px solid #eee; color: #999; }}
        .sub-card.self-excluded {{ background: #ffebee; border: 2px solid #f44336; }}
        .sub-card .sub-name {{ font-weight: 600; }}
        .sub-card .sub-info {{ font-size: 10px; margin-top: 3px; color: #666; }}
    </style>
</head>
<body>
    <div class="container">
        <h1>moqx Top-N Filtering Timeline</h1>
        <p class="subtitle" id="subtitle">Loading...</p>

        <div class="controls">
            <label>Publisher:</label>
            <div id="pub-buttons"></div>
            <div class="time-display" id="time-display">Time: 0.0s</div>
        </div>

        <div class="timeline-container">
            <div class="timeline-header">
                <h3 id="pub-title">Publisher Timelines</h3>
                <div class="legend">
                    <div class="legend-item"><div class="legend-box" style="background:#bdbdbd"></div> Silent (0)</div>
                    <div class="legend-item"><div class="legend-box" style="background:#ff9800"></div> Speech Start (2)</div>
                    <div class="legend-item"><div class="legend-box" style="background:#4caf50"></div> Speaking (1)</div>
                </div>
            </div>
            <div class="stacked-timelines" id="stacked-timelines"></div>

            <div class="block-detail" id="block-detail">
                <h4>Click or drag on a timeline to inspect</h4>
            </div>

            <div class="subscriber-section">
                <h4 id="sub-header">Subscriber visibility:</h4>
                <div class="subscriber-grid" id="subscriber-grid"></div>
            </div>

            <div class="subscriber-section" style="margin-top:20px">
                <h4>Selection/Eviction Log (relay decisions)</h4>
                <div id="sel-log" style="max-height:250px;overflow-y:auto;font-family:monospace;font-size:12px;
                    background:#f8f9fa;padding:10px;border-radius:6px;border:1px solid #ddd;line-height:1.8"></div>
            </div>
        </div>
    </div>

    <script>
    const rawData = {data_json};
    const testData = {{
        duration: rawData.duration || 10000,
        topN: rawData.top_n || 5,
        publishers: rawData.publishers || [],
        subscribers: rawData.subscribers || [],
        queries: rawData.queries || [],
        selectionEvents: rawData.selection_events || []
    }};

    let selectedPublisher = testData.publishers.length > 0 ? testData.publishers[0].id : 0;
    let currentTime = 0;
    let isDragging = false;

    function getValueClass(v) {{ return v === 0 ? 'silent' : v >= 2 ? 'start' : 'speaking'; }}

    function getPublisherValue(pubId, time) {{
        const pub = testData.publishers.find(p => p.id === pubId);
        if (!pub) return 0;
        for (let i = pub.timeline.length - 1; i >= 0; i--) {{
            if (time >= pub.timeline[i].start) return pub.timeline[i].value;
        }}
        return 0;
    }}

    function computeTopN(time, excludePubId) {{
        let values = testData.publishers.map(p => ({{ id: p.id, value: getPublisherValue(p.id, time) }}));
        if (excludePubId != null) values = values.filter(v => v.id !== excludePubId);
        values.sort((a, b) => b.value - a.value || a.id - b.id);
        return values.slice(0, testData.topN).map(v => v.id);
    }}

    function canSubscriberSee(sub, pubId, time) {{
        if (sub.is_publisher && sub.publisher_id === pubId) return 'self-excluded';
        const excludeId = sub.is_publisher ? sub.publisher_id : null;
        const topN = computeTopN(time, excludeId);
        return topN.includes(pubId) ? 'sees' : 'not-sees';
    }}

    function selectPublisher(pubId) {{
        selectedPublisher = pubId;
        document.querySelectorAll('.pub-btn').forEach(btn => {{
            btn.classList.toggle('active', parseInt(btn.dataset.pubId) === pubId);
        }});
        renderTimeline();
    }}

    function setTime(time) {{
        currentTime = Math.max(0, Math.min(time, testData.duration));
        document.getElementById('time-display').textContent = `Time: ${{(currentTime/1000).toFixed(1)}}s`;
        renderBlockDetail();
        renderSubscribers();
        document.querySelectorAll('.scrubber').forEach(s => {{
            s.style.left = `${{(currentTime / testData.duration) * 100}}%`;
        }});
    }}

    function renderPubButtons() {{
        const c = document.getElementById('pub-buttons');
        c.innerHTML = '';
        testData.publishers.forEach(pub => {{
            const btn = document.createElement('button');
            btn.className = 'pub-btn' + (pub.id === selectedPublisher ? ' active' : '');
            btn.textContent = `P${{pub.id}}`;
            btn.title = pub.track;
            btn.dataset.pubId = pub.id;
            btn.onclick = () => selectPublisher(pub.id);
            c.appendChild(btn);
        }});
    }}

    function renderTimeline() {{
        const pub = testData.publishers.find(p => p.id === selectedPublisher);
        if (!pub) return;
        document.getElementById('pub-title').textContent = `Publisher Timelines (selected: P${{pub.id}} - ${{pub.track}})`;
        document.getElementById('sub-header').textContent = `Who sees P${{pub.id}}:`;

        const container = document.getElementById('stacked-timelines');
        container.innerHTML = '';

        testData.publishers.forEach(p => {{
            const isSelected = p.id === selectedPublisher;
            const row = document.createElement('div');
            row.className = 'stacked-row' + (isSelected ? ' selected' : '');

            const label = document.createElement('div');
            label.className = 'stacked-label' + (isSelected ? ' selected' : '');
            label.textContent = `P${{p.id}} ${{p.track.substring(0, 8)}}`;
            label.title = p.track;
            row.appendChild(label);

            const track = document.createElement('div');
            track.className = 'timeline-track' + (isSelected ? ' selected' : '');
            track.innerHTML = '<div class="scrubber" style="left:0%"></div>';

            if (p.timeline.length === 0) {{
                const div = document.createElement('div');
                div.className = 'time-block silent';
                div.style.width = '100%';
                track.appendChild(div);
            }} else {{
                p.timeline.forEach(block => {{
                    const width = ((block.end - block.start) / testData.duration) * 100;
                    const div = document.createElement('div');
                    div.className = `time-block ${{getValueClass(block.value)}}`;
                    div.style.width = `${{Math.max(width, 0.3)}}%`;
                    if (isSelected && width > 3) {{
                        div.innerHTML = `<span class="value-label">${{block.value}}</span>`;
                    }}
                    track.appendChild(div);
                }});
            }}

            const handleMouse = (e) => {{
                const rect = track.getBoundingClientRect();
                const x = Math.max(0, Math.min(e.clientX - rect.left, rect.width));
                setTime((x / rect.width) * testData.duration);
            }};
            track.onmousedown = (e) => {{ isDragging = true; handleMouse(e); }};
            track.onmousemove = (e) => {{ if (isDragging) handleMouse(e); }};

            row.appendChild(track);
            container.appendChild(row);
        }});

        document.onmouseup = () => {{ isDragging = false; }};
        setTime(currentTime);
    }}

    function renderBlockDetail() {{
        const value = getPublisherValue(selectedPublisher, currentTime);
        let sees = 0, notSees = 0, excluded = 0;
        testData.subscribers.forEach(sub => {{
            const s = canSubscriberSee(sub, selectedPublisher, currentTime);
            if (s === 'sees') sees++;
            else if (s === 'self-excluded') excluded++;
            else notSees++;
        }});

        const allVals = testData.publishers.map(p => ({{
            id: p.id, value: getPublisherValue(p.id, currentTime)
        }})).sort((a,b) => b.value - a.value || a.id - b.id);

        document.getElementById('block-detail').innerHTML = `
            <h4>T=${{(currentTime/1000).toFixed(1)}}s: P${{selectedPublisher}} value=${{value}} (${{getValueClass(value)}})</h4>
            <div class="stats">
                <div class="stat"><div class="stat-value">${{sees}}</div><div class="stat-label">Can See</div></div>
                <div class="stat"><div class="stat-value" style="color:#f44336">${{excluded}}</div><div class="stat-label">Self-Excluded</div></div>
                <div class="stat"><div class="stat-value" style="color:#999">${{notSees}}</div><div class="stat-label">Can't See</div></div>
            </div>
            <div style="font-size:12px;color:#555">
                <strong>Ranking:</strong> ${{allVals.slice(0, 10).map(v => `P${{v.id}}=${{v.value}}`).join(', ')}}${{allVals.length > 10 ? '...' : ''}}
            </div>`;
    }}

    function renderSubscribers() {{
        const grid = document.getElementById('subscriber-grid');
        grid.innerHTML = '';
        // Show max 50 subscribers to keep UI responsive
        const displaySubs = testData.subscribers.slice(0, 50);
        displaySubs.forEach(sub => {{
            const status = canSubscriberSee(sub, selectedPublisher, currentTime);
            const card = document.createElement('div');
            card.className = `sub-card ${{status}}`;
            const excludeId = sub.is_publisher ? sub.publisher_id : null;
            const topN = computeTopN(currentTime, excludeId);
            let info = status === 'self-excluded'
                ? `Self (P${{sub.publisher_id}})<br>Sees: [${{topN.map(id=>'P'+id).join(',')}}]`
                : `Top-${{testData.topN}}: [${{topN.map(id=>'P'+id).join(',')}}]`;
            const label = sub.is_publisher ? `S${{sub.id}} (=P${{sub.publisher_id}})` : `S${{sub.id}}`;
            card.innerHTML = `<div class="sub-name">${{label}}</div><div class="sub-info">${{info}}</div>`;
            grid.appendChild(card);
        }});
        if (testData.subscribers.length > 50) {{
            const more = document.createElement('div');
            more.className = 'sub-card not-sees';
            more.innerHTML = `<div class="sub-name">+${{testData.subscribers.length - 50}} more</div>`;
            grid.appendChild(more);
        }}
    }}

    function renderSelectionLog() {{
        const logEl = document.getElementById('sel-log');
        if (!testData.selectionEvents.length) {{
            logEl.innerHTML = '<span style="color:#999">No selection events (relay did not emit top_n_selected/evicted)</span>';
            return;
        }}
        // Show last 100 events
        const recent = testData.selectionEvents.slice(-100);
        logEl.innerHTML = recent.map(ev => {{
            const ts = (ev.ts_ms / 1000).toFixed(2);
            const color = ev.action === 'selected' ? '#2e7d32' : '#c62828';
            const sym = ev.action === 'selected' ? '+' : '-';
            return `<div style="color:${{color}}">[T=${{ts}}s] ${{sym}} S${{ev.subscriber_id}} ${{ev.action}} <b>${{ev.track}}</b> (P${{ev.selected[0]}})</div>`;
        }}).join('');
        logEl.scrollTop = logEl.scrollHeight;
    }}

    // Init
    document.getElementById('subtitle').textContent =
        `${{testData.publishers.length}} Publishers, ${{testData.subscribers.length}} Subscribers, Top-${{testData.topN}} (${{(testData.duration/1000).toFixed(1)}}s)`;
    if (testData.publishers.length > 0) {{
        renderPubButtons();
        renderTimeline();
        renderSelectionLog();
    }} else {{
        document.getElementById('block-detail').innerHTML = '<h4>No TOPN_EVENT data found.</h4>';
    }}
    </script>
</body>
</html>"""


def main():
    parser = argparse.ArgumentParser(description="Generate top-N timeline visualization from TOPN_EVENT logs")
    parser.add_argument("input", help="TOPN_EVENT log file")
    parser.add_argument("-o", "--output", help="Output HTML path (default: <input>.html)")
    parser.add_argument("--open", action="store_true", help="Open in browser after generating")
    args = parser.parse_args()

    if not Path(args.input).exists():
        print(f"Error: {args.input} not found", file=sys.stderr)
        sys.exit(1)

    output = args.output or str(Path(args.input).with_suffix(".html"))

    print(f"Reading events from: {args.input}")
    events = parse_events(args.input)
    print(f"Parsed {len(events)} events")

    if not events:
        print("No TOPN_EVENT entries found. Make sure the perf test was run with --perf_event_log.", file=sys.stderr)
        sys.exit(1)

    viz_data = process_events(events)
    html = generate_html(viz_data)

    with open(output, "w") as f:
        f.write(html)
    print(f"Generated: {output}")
    print(f"  Publishers: {len(viz_data['publishers'])}")
    print(f"  Subscribers: {len(viz_data['subscribers'])}")
    print(f"  Queries: {len(viz_data['queries'])}")
    print(f"  Duration: {viz_data['duration']/1000:.1f}s")

    if args.open:
        if platform.system() == "Darwin":
            subprocess.run(["open", output])
        elif platform.system() == "Linux":
            subprocess.run(["xdg-open", output])
        else:
            print(f"Open {output} in your browser")


if __name__ == "__main__":
    main()
