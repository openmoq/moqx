#!/usr/bin/env python3
"""test_relay_fetch_credit.py — a relay serves fetches beyond its bidi limit.

Draft 18 carries every request on its own client-initiated QUIC bidi stream
with no MoQT request-id cap, so the QUIC bidi stream limit (moqx default: 16)
is the only backpressure on concurrent requests. A relay forwards each
subscriber's joining fetch as a FETCH on its single long-lived upstream
session; serving a long run of subscribers therefore depends on each completed
fetch releasing its request stream and returning the QUIC MAX_STREAMS credit
it held.

The test drives a chain (A upstream, B downstream): a publisher at A and a run
of identical joining-fetch subscribers at B, more than the bidi limit. It
asserts B forwards and serves every one.

The A↔B link must run draft 18 for those per-request streams to exist. It
negotiates the highest draft both relays support (UpstreamProvider /
buildAlpns), so advertising 18 on both is enough. Over draft 16 the requests
share the control stream and the limit never applies.

Usage: python3 test/test_relay_fetch_credit.py [path/to/moqx] [--save-logs [DIR]]
"""

import os

from lib.moq_harness import main

NAMESPACE = "moq-date"
TRACK = "date"

# Past moqx's default initial_max_streams_bidi (16), with margin for the
# peering and subscribe streams, so serving all of them needs credit returned
# as earlier fetches complete.
FETCHES = 22

TIMEOUT = 3  # seconds each subscriber listens before exiting


def run(h):
    # Per-request bidi streams are a draft-18 feature, and draft 18 is opt-in
    # (test_versions.sh pins the interop default). Advertise 16+18 so the chain
    # negotiates 18 via ALPN.
    h.moqt_versions = "[16, 18]"

    h.relay("A")
    h.relay("B", upstream="A")
    h.start()

    h.case(f"{FETCHES} sequential joining fetches forwarded on one upstream session")

    pub = h.actor("pub", "publisher", relay="A", ns=NAMESPACE, track=TRACK)
    pub.start()
    h.wait_namespace("B", NAMESPACE)

    for i in range(FETCHES):
        sub = h.actor(
            f"sub{i}",
            "subscriber",
            relay="B",
            ns=NAMESPACE,
            track=TRACK,
            flags="--jrfetch --join_start=1",
            timeout=TIMEOUT,
        )
        sub.run()

    # Credit comes back as each fetch completes, so B serves every forwarded
    # fetch. A completed fetch that never released its upstream request stream
    # would starve B's credit and fail the rest.
    h.expect_metric("B", "pubFetchSuccess_total", "eq", FETCHES)
    h.expect_metric("B", "pubFetchError_total", "eq", 0)

    pub.stop()
    h.expect_relay_alive("A")
    h.expect_relay_alive("B")


# Not registered with ctest. A ctest regression test would claim a base in
# test_ports.sh instead of this default — the env override still wins, keeping
# parallel runs safe.
os.environ.setdefault("HARNESS_BASE_PORT", "19850")
main(run, base_port_key="fetch_credit")
