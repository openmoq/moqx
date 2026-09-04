#!/usr/bin/env python3
"""test_relay_pyramid.py — data fan-out across a branching relay tree.

Topology:
        A          root, no upstream
       / \\
      B   C        both peer up to A
      |
      D            peers up to B

A publisher attaches to the root; a subscriber attaches to every relay. All
four must receive the data, which exercises forwarding one hop (B, C) and two
hops (D) from the origin. Then B is killed to confirm the loss is contained:
D loses its upstream while A and C stay healthy.

Usage: python3 test/test_relay_pyramid.py [path/to/moqx] [--save-logs [DIR]]
"""

from lib.moq_harness import main

NAMESPACE = "live"
TIMEOUT = 2


def run(h):
    h.relay("A")
    h.relay("B", upstream="A")
    h.relay("C", upstream="A")
    h.relay("D", upstream="B")
    # start() derives the peering waits from the topology: A must accept 2
    # sessions (B and C), B must accept 1 (D). No hand-counted session numbers.
    h.start()

    # ── Fan-out ────────────────────────────────────────────────────────────────
    h.case("Data published at the root reaches every relay in the tree")

    pub = h.actor("pub", "publisher", relay="A", ns=NAMESPACE, track="date")
    pub.start()
    h.wait_sessions_atleast("A", "+1")

    # The deepest hop implies the shallower ones: D only learns the namespace
    # via B, which only learns it via A.
    h.wait_namespace("D", NAMESPACE)

    for name in ("A", "B", "C", "D"):
        sub = h.actor(
            "sub_" + name,
            "subscriber",
            relay=name,
            ns=NAMESPACE,
            track="date",
            timeout=TIMEOUT,
        )
        sub.run()
        h.expect_received(sub)
        h.expect_no_errors(sub)

    h.expect_peer_count("A", "ge", 2)  # B and C
    h.expect_peer_count("B", "ge", 1)  # D
    h.expect_upstream_state("B", "connected")
    h.expect_upstream_state("C", "connected")
    h.expect_upstream_state("D", "connected")

    # Session accounting across the tree: A holds B, C and the publisher; B
    # holds only D. A relay counts its inbound peers, not its own outbound
    # upstream.
    h.expect_metric("A", "moqActiveSessions", "ge", 3)
    h.expect_metric("B", "moqActiveSessions", "ge", 1)
    h.expect_metric("D", "moqActiveSessions", "eq", 0)

    # ── Containment ────────────────────────────────────────────────────────────
    # The publisher stays up across the kill so the surviving branch has live
    # data.
    h.case("Forceful loss of B leaves the A-C branch serving data")

    h.relay_kill("B")  # marks B exempt from the clean-exit check

    h.expect_relay_alive("A")
    h.expect_relay_alive("C")
    h.expect_relay_alive("D")  # D survives losing its upstream, it does not crash

    # The real containment check: C can still serve a new subscriber from the
    # root. Asserting on A's peer_count here would be vacuous — A does not
    # notice a SIGKILLed peer until idle_timeout_ms (60s), well past the end of
    # this test.
    sub_after = h.actor(
        "sub_after",
        "subscriber",
        relay="C",
        ns=NAMESPACE,
        track="date",
        timeout=TIMEOUT,
    )
    sub_after.run()
    h.expect_received(sub_after)
    h.expect_no_errors(sub_after)

    pub.stop()


main(run, base_port_key="pyramid")
