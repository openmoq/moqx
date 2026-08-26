#!/usr/bin/env python3
"""test_relay_hops_cycle.py — RELAY_HOPS stabilizes A -> B -> C -> A.

A namespace published at A must reach every relay in the cycle and then stop
changing: relay-hops drops an advertisement whose HOP_PATH already contains
the local Hop ID, so the tree converges instead of circulating forever.
See docs/relay-hops.md.

Usage: python3 test/test_relay_hops_cycle.py [path/to/moqx] [--save-logs [DIR]]
"""

from lib.moq_harness import main

NAMESPACE = "relay-hop-cycle"


def run(h):
    h.relay("A", upstream="B", relay_id="cycle-a")
    h.relay("B", upstream="C", relay_id="cycle-b")
    h.relay("C", upstream="A", relay_id="cycle-c")
    h.start()

    h.case("Namespace published at A propagates around the cycle and stabilizes")

    pub = h.actor("pub", "publisher", relay="A", ns=NAMESPACE, track="date")
    pub.start()

    # These are the propagation assertion: each aborts the run if the namespace
    # never arrives, so reaching the stability check proves it reached all three.
    h.wait_namespace("A", NAMESPACE)
    h.wait_namespace("B", NAMESPACE)
    h.wait_namespace("C", NAMESPACE)

    h.expect_settled(2)

    h.expect_upstream_state("A", "connected")
    h.expect_upstream_state("B", "connected")
    h.expect_upstream_state("C", "connected")

    # Each relay in the cycle holds exactly one peer session; A also has the
    # publisher. Anything higher would mean the cycle re-dialed and stacked up
    # duplicate sessions.
    h.expect_metric("A", "moqActiveSessions", "eq", 2)
    h.expect_metric("B", "moqActiveSessions", "eq", 1)
    h.expect_metric("C", "moqActiveSessions", "eq", 1)
    h.expect_relay_alive("A")
    h.expect_relay_alive("B")
    h.expect_relay_alive("C")

    pub.stop()


main(run, base_port_key="hops")
