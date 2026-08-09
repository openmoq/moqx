#!/usr/bin/env python3
"""Generate Prometheus file_sd targets for per-track metric scrapes.

/metrics/track takes one namespace per request and rejects a match wider than
its limit, so each namespace is scraped separately. This walks the relay's
namespace tree and writes one target per namespace; Prometheus rereads the file
without a restart.

Namespaces are written in the moq-transport safe form the endpoint expects:
[A-Za-z0-9_] passes through, every other byte becomes .<hex>, and tuple
elements are joined with '-'.
"""
import json
import os
import sys
import tempfile
import time
import urllib.request

STATE_URL = os.environ.get("MOQX_STATE_URL", "http://moqx:8000/state")
OUT_PATH = os.environ.get("MOQX_TARGETS_PATH", "/targets/namespaces.json")
INTERVAL = float(os.environ.get("MOQX_TARGETS_INTERVAL", "30"))

_PASS = set(
    "abcdefghijklmnopqrstuvwxyz" "ABCDEFGHIJKLMNOPQRSTUVWXYZ" "0123456789" "_"
)


def safe_element(element):
    out = []
    for byte in element.encode():
        ch = chr(byte)
        out.append(ch if ch in _PASS else ".%02x" % byte)
    return "".join(out)


def safe_namespace(tuple_elements):
    return "-".join(safe_element(e) for e in tuple_elements)


def walk(node, found):
    """Collect every node carrying a namespace, not just the leaves: tracks can
    be published at any depth."""
    full = node.get("full_namespace") or []
    if full:
        found.append(full)
    for child in (node.get("children") or {}).values():
        walk(child, found)


def namespaces():
    with urllib.request.urlopen(STATE_URL, timeout=10) as resp:
        state = json.load(resp)
    found = []
    for service in (state.get("services") or {}).values():
        tree = service.get("namespace_tree")
        if tree:
            walk(tree, found)
    # A namespace can appear under more than one service.
    return sorted({tuple(ns) for ns in found})


def write(path, entries):
    payload = [
        {"targets": [safe_namespace(ns)], "labels": {"moqx_namespace": "/".join(ns)}}
        for ns in entries
    ]
    body = json.dumps(payload, indent=2) + "\n"
    if os.path.exists(path) and open(path).read() == body:
        return False
    # Rename into place so Prometheus never reads a partial file.
    directory = os.path.dirname(path) or "."
    fd, tmp = tempfile.mkstemp(dir=directory)
    with os.fdopen(fd, "w") as handle:
        handle.write(body)
    os.replace(tmp, path)
    return True


def main():
    while True:
        try:
            entries = namespaces()
            if write(OUT_PATH, entries):
                print("wrote %d namespace target(s)" % len(entries), flush=True)
        except Exception as exc:  # keep polling: the relay restarts
            print("namespace target refresh failed: %s" % exc, file=sys.stderr, flush=True)
        time.sleep(INTERVAL)


if __name__ == "__main__":
    main()
