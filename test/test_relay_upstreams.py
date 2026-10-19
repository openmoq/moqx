#!/usr/bin/env python3
"""test_relay_upstreams.py — one service peers with several upstreams.

A lists B and C under `upstreams`. Each of B and C carries its own publisher,
and subscribers on A must reach both through their own links.

Usage: python3 test/test_relay_upstreams.py [path/to/moqx] [--save-logs [DIR]]
"""

from lib.moq_harness import main

NS_B = "upstream-b"
NS_C = "upstream-c"
TIMEOUT = 2  # seconds to wait for data


def run(h):
    h.relay("A", upstreams=["B", "C"])
    h.relay("B")
    h.relay("C")
    h.start()

    h.case("A peers with both upstreams")
    h.wait_upstream_state("A", "connected", index=0)
    h.wait_upstream_state("A", "connected", index=1)

    h.case("Subscribers on A reach a publisher on each upstream")
    pub_b = h.actor("pub-b", "publisher", relay="B", ns=NS_B, track="date")
    pub_c = h.actor("pub-c", "publisher", relay="C", ns=NS_C, track="date")
    pub_b.start()
    pub_c.start()
    h.wait_namespace("A", NS_B)
    h.wait_namespace("A", NS_C)

    for ns in (NS_B, NS_C):
        sub = h.actor(
            f"sub-{ns}", "subscriber", relay="A", ns=ns, track="date", timeout=TIMEOUT
        )
        sub.run()
        h.expect_received(sub)
        h.expect_no_errors(sub)

    pub_b.stop()
    pub_c.stop()


main(run, base_port_key="upstreams")
