#!/usr/bin/env python3
"""test_relay_cluster_diamond.py — MoQ Cluster picks the cheaper of two paths.

    pub -> A -+- B ------+- D <- sub
              +- E -- C -+

B declares RELAY_COST 5 on its link to D and C declares 1, so D reaches A's
namespace more cheaply through C even though that path is a hop longer. A
subscriber on D must be served through C. Once C is gone, a new subscriber on
D must be served through B. See docs/moq-cluster.md.

Usage: python3 test/test_relay_cluster_diamond.py [path/to/moqx] [--save-logs [DIR]]
"""

from lib.moq_harness import main

NAMESPACE = "cluster-diamond"
TRACK = "date"
TIMEOUT = 4  # seconds each subscriber runs


def run(h):
    h.relay("A")
    h.relay("B", upstreams=["A", ("D", 5)])
    h.relay("E", upstream="A")
    h.relay("C", upstreams=["E", ("D", 1)])
    h.relay("D")
    h.start()

    pub = h.actor("pub", "publisher", relay="A", ns=NAMESPACE, track=TRACK)
    pub.start()
    for name in ("B", "C", "D", "E"):
        h.wait_namespace(name, NAMESPACE)
    h.wait_routes("D", NAMESPACE, 2)
    h.wait_namespace_route("D", NAMESPACE, via="C")

    h.case("A subscription on D takes the cheaper, longer path through C")
    sub = h.actor(
        "sub", "subscriber", relay="D", ns=NAMESPACE, track=TRACK, timeout=TIMEOUT
    )
    sub.start()
    h.wait_subscription("C", NAMESPACE, TRACK)
    h.expect_no_subscription("B", NAMESPACE, TRACK)
    sub.wait()
    h.expect_received(sub)
    h.expect_no_errors(sub)

    h.case("Without C, a new subscription on D goes through B")
    h.relay_stop("C")
    h.wait_peer_count("D", 1)
    h.wait_namespace_route("D", NAMESPACE, via="B")
    sub2 = h.actor(
        "sub2", "subscriber", relay="D", ns=NAMESPACE, track=TRACK, timeout=TIMEOUT
    )
    sub2.start()
    h.wait_subscription("B", NAMESPACE, TRACK)
    sub2.wait()
    h.expect_received(sub2)
    h.expect_no_errors(sub2)

    for name in ("A", "B", "D", "E"):
        h.expect_relay_alive(name)
    pub.stop()


main(run, base_port_key="cluster")
