#!/usr/bin/env python3
"""test_relay_chain.py — end-to-end relay chaining test.

Two moqx instances (A upstream, B downstream peered to A), a moqdateserver
publishing, and a moqtextclient subscribing. Exercises data flow in both
directions across the chain, plus joining fetch and --publish push mode.

Requires draft 16+ for relay peering (wildcard subscribeNamespace).
moqdateserver and moqtextclient come from the moxygen install bin, resolved
from the build (MOQBIN overrides).

Usage: python3 test/test_relay_chain.py [path/to/moqx] [--save-logs [DIR]]
  --save-logs [DIR]  Save relay DBG4 logs; DIR defaults to
                     .scratch/moq_harness_logs/test_relay_chain
"""

from lib.moq_harness import main

NAMESPACE = "moq-date"
NAMESPACE2 = "moq-date-2"
NAMESPACE3 = "moq-publish"  # for --publish push-mode test
TIMEOUT = 2  # seconds to wait for data


def run(h):
    h.relay("A", relay_id="upstream-test")
    h.relay("B", upstream="A", relay_id="downstream-test")
    h.start()

    # ── Direction 1: publisher → A, subscriber via B ───────────────────────────
    h.case("Direction 1: publish to A, subscribe via B")

    pub = h.actor("pub", "publisher", relay="A", ns=NAMESPACE, track="date")
    sub = h.actor(
        "sub", "subscriber", relay="B", ns=NAMESPACE, track="date", timeout=TIMEOUT
    )

    pub.start()
    h.wait_sessions_atleast("A", "+1")
    sub.run()

    h.expect_received(sub)
    h.expect_no_errors(sub)

    # /state snapshot while the publisher is active: A carries the publish
    # subscription and the namespace, and sees B as a downstream peer; B reports
    # its upstream connected.
    h.expect_relay_id("A", "upstream-test")
    h.expect_relay_id("B", "downstream-test")
    h.expect_namespace_present("A", NAMESPACE)
    h.expect_peer_count("A", "ge", 1)
    h.expect_upstream_state("B", "connected")

    # A carries two sessions here: the downstream peer B, and the publisher.
    # B carries none: a relay counts inbound sessions, and B's link to A is its
    # own outbound upstream. (The pyramid test pins the same rule on D.)
    h.expect_metric("A", "moqActiveSessions", "ge", 2)
    h.expect_metric("B", "moqActiveSessions", "eq", 0)

    # ── Direction 2: publisher → B, subscriber via A ───────────────────────────
    h.case("Direction 2: publish to B, subscribe via A")

    pub2 = h.actor("pub2", "publisher", relay="B", ns=NAMESPACE2, track="date")
    sub2 = h.actor(
        "sub2", "subscriber", relay="A", ns=NAMESPACE2, track="date", timeout=TIMEOUT
    )

    pub2.start()
    h.wait_sessions_atleast("B", "+1")
    sub2.run()

    h.expect_received(sub2)
    h.expect_no_errors(sub2)

    # ── Direction 3: joining fetch via the chain ───────────────────────────────
    # pub from direction 1 is still publishing NAMESPACE on A.
    # --jrfetch --join_start=1 fetches one group back while subscribing forward,
    # exercising both fetch and subscribe forwarding through the chain.
    h.case("Direction 3: joining fetch via B")

    sub3 = h.actor(
        "sub3",
        "subscriber",
        relay="B",
        ns=NAMESPACE,
        track="date",
        flags="--jrfetch --join_start=1",
        timeout=TIMEOUT,
    )

    sub3.run()

    h.expect_received(sub3)
    h.expect_no_errors(sub3)

    pub.stop()

    # ── Direction 4: --publish push mode through the chain ─────────────────────
    # textclient --publish registers subscribeNamespace on B; dateserver
    # --publish pushes via PUBLISH to A; the chain routes it down to the
    # textclient.
    h.case("Direction 4: --publish push mode, A to B")

    sub4 = h.actor(
        "sub4",
        "subscriber",
        relay="B",
        ns=NAMESPACE3,
        track="date",
        flags="--publish",
        timeout=TIMEOUT,
    )
    pub4 = h.actor(
        "pub4", "publisher", relay="A", ns=NAMESPACE3, track="date", flags="--publish"
    )

    # pub2 is deliberately still running: it holds B at one session, so waiting
    # for two is a real "sub4 registered its subscribeNamespace" signal.
    # Stopping pub2 first would make this wait pass on pub2's not-yet-torn-down
    # session — relay side teardown is async — and pub4 would then push before
    # sub4 was listening.
    sub4.start()
    h.wait_sessions_atleast("B", "+2")
    pub4.start()
    sub4.wait()

    h.expect_received(sub4)

    pub4.stop()
    pub2.stop()

    h.expect_relay_alive("A")
    h.expect_relay_alive("B")


main(run, base_port_key="chain")
