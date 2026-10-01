#!/usr/bin/env python3
"""Generate Prometheus file_sd targets for per-track metric scrapes.

/metrics/track takes one namespace per request and rejects a match wider than
its limit, so each namespace is scraped separately. This walks the relay's
namespace tree and writes one target per namespace; Prometheus rereads the file
without a restart.

Namespaces are written in the moq-transport safe form the endpoint expects:
[A-Za-z0-9_] passes through, every other byte becomes .<hex>, and tuple
elements are joined with '-'.

It also serves /metrics with moqx_namespace_origin{namespace, origin}: who
publishes each namespace, as the peer relay's ID or else the publisher's IP.
The value is when that origin was last seen, so the newest one wins a join.
"""

import http.server
import json
import os
import sys
import tempfile
import threading
import time
import urllib.request

STATE_URL = os.environ.get("MOQX_STATE_URL", "http://moqx:8000/state")
OUT_PATH = os.environ.get("MOQX_TARGETS_PATH", "/targets/namespaces.json")
INTERVAL = float(os.environ.get("MOQX_TARGETS_INTERVAL", "30"))
ORIGIN_PORT = int(os.environ.get("MOQX_ORIGIN_PORT", "9102"))

_PASS = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")

_metrics = b""
_metrics_lock = threading.Lock()


def safe_element(element):
    out = []
    for byte in element.encode():
        ch = chr(byte)
        out.append(ch if ch in _PASS else f".{byte:02x}")
    return "".join(out)


def safe_namespace(tuple_elements):
    return "-".join(safe_element(e) for e in tuple_elements)


def top_level(tree):
    """Only the tree's immediate children.

    The endpoint matches a namespace prefix, so scraping both a parent and its
    child returns the same tracks twice and every aggregate double-counts.
    Top-level namespaces do not overlap each other and their prefixes still
    cover every track beneath them.
    """
    out = []
    for child in (tree.get("children") or {}).values():
        full = child.get("full_namespace") or []
        if full:
            out.append(full)
    return out


def fetch_state():
    with urllib.request.urlopen(STATE_URL, timeout=10) as resp:
        return json.load(resp)


def namespaces(state):
    found = []
    for service in (state.get("services") or {}).values():
        tree = service.get("namespace_tree")
        if tree:
            found.extend(top_level(tree))
    # A namespace can appear under more than one service.
    return sorted({tuple(ns) for ns in found})


def host_of(address):
    """Drop the port: '192.0.2.7:5000' -> '192.0.2.7', '[2001:db8::7]:5000' -> '2001:db8::7'."""
    if address.startswith("["):
        return address[1 : address.find("]")]
    return address.rsplit(":", 1)[0]


def origins(state):
    """Safe-form namespace -> peer relay ID or publisher IP.

    Tracks the relay pulls from an upstream relay have no publisher in the
    tree; their subscription's source address stands in.
    """
    found = {}

    def walk(node):
        full = node.get("full_namespace") or []
        origin = node.get("peer_id") or host_of(node.get("publisher") or "")
        if full and origin:
            found[safe_namespace(full)] = origin
        for child in (node.get("children") or {}).values():
            walk(child)

    for service in (state.get("services") or {}).values():
        if service.get("namespace_tree"):
            walk(service["namespace_tree"])
        for sub in service.get("subscriptions") or []:
            if sub.get("namespace") and sub.get("source_address"):
                found.setdefault(
                    safe_namespace(sub["namespace"]), host_of(sub["source_address"])
                )
    return found


def label(value):
    return value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def render(origin_map, now):
    lines = [
        "# HELP moqx_namespace_origin Peer relay ID or publisher IP per namespace; value is when it was last seen.",
        "# TYPE moqx_namespace_origin gauge",
    ]
    for ns, origin in sorted(origin_map.items()):
        lines.append(
            f'moqx_namespace_origin{{namespace="{label(ns)}",origin="{label(origin)}"}} {now:.0f}'
        )
    return ("\n".join(lines) + "\n").encode()


class MetricsHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/metrics":
            self.send_error(404)
            return
        with _metrics_lock:
            body = _metrics
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; version=0.0.4")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def write(path, entries):
    payload = [{"targets": [safe_namespace(ns)]} for ns in entries]
    body = json.dumps(payload, indent=2) + "\n"
    if os.path.exists(path) and open(path).read() == body:
        return False
    # Rename into place so Prometheus never reads a partial file.
    directory = os.path.dirname(path) or "."
    fd, tmp = tempfile.mkstemp(dir=directory)
    with os.fdopen(fd, "w") as handle:
        handle.write(body)
    # mkstemp makes it 0600 and this runs as root; Prometheus runs as nobody.
    os.chmod(tmp, 0o644)
    os.replace(tmp, path)
    return True


def main():
    global _metrics
    server = http.server.ThreadingHTTPServer(("", ORIGIN_PORT), MetricsHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    while True:
        try:
            state = fetch_state()
            entries = namespaces(state)
            if write(OUT_PATH, entries):
                print(f"wrote {len(entries)} namespace target(s)", flush=True)
            body = render(origins(state), time.time())
            with _metrics_lock:
                _metrics = body
        except Exception as exc:  # keep polling: the relay restarts
            print(
                f"namespace target refresh failed: {exc}", file=sys.stderr, flush=True
            )
        time.sleep(INTERVAL)


if __name__ == "__main__":
    main()
