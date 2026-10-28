#!/usr/bin/env python3
"""test_relay_sighup.py — SIGHUP leaves the relay and its sessions running.

Usage: python3 test/test_relay_sighup.py [path/to/moqx] [--save-logs [DIR]]
"""

import signal

from lib.moq_harness import main

NAMESPACE = "moq-date"
TIMEOUT = 2  # seconds to wait for data


def run(h):
    h.relay("A")
    h.start()

    pub = h.actor("pub", "publisher", relay="A", ns=NAMESPACE, track="date")
    pub.start()
    h.wait_namespace("A", NAMESPACE)

    h.case("A publisher connected before SIGHUP still serves a new subscriber")
    h.relay_signal("A", signal.SIGHUP)
    sub = h.actor(
        "sub", "subscriber", relay="A", ns=NAMESPACE, track="date", timeout=TIMEOUT
    )
    sub.run()
    h.expect_received(sub)
    h.expect_no_errors(sub)

    # sub.run() took TIMEOUT seconds, the window for the relay to handle SIGHUP.
    h.case("The relay logs SIGHUP and keeps running")
    h.expect_relay_log("A", "Ignoring SIGHUP")
    h.expect_relay_alive("A")
    h.expect_metric("A", "moqActiveSessions", "ge", 1)

    pub.stop()


main(run, base_port_key="sighup")
