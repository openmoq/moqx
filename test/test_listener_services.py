#!/usr/bin/env python3
"""test_listener_services.py — services bound to a subset of listeners.

Relay A has four listeners. Service "default" is bound to "public" and
"internal"; service "ops" holds the identical match rule on "ops" only.
Relay B peers to A through "public".

- A publisher on "internal" reaches a subscriber on "public" (one shared relay).
- A publisher on "ops" reaches a subscriber on "ops" but not one on "public":
  the listener, not the rule, picks the service.
- A subscriber on "isolated" is rejected at setup, over WebTransport and (mvfst
  only) native QUIC: no service is bound there.
- /config reports the binding; /state reports the peer's listener.

Usage: python3 test/test_listener_services.py [path/to/moqx] [--save-logs [DIR]]
"""

from lib.moq_harness import main

NAMESPACE = "moq-date"
OPS_NAMESPACE = "moq-ops"
TIMEOUT = 2  # seconds to wait for data
NO_SERVICE = r"No service matched on listener 'isolated'"


def run(h):
    h.relay(
        "A",
        relay_id="listeners-test",
        listeners=["public", "internal", "ops", "isolated"],
        service_listeners=["public", "internal"],
        extra_services={"ops": ["ops"]},
    )
    h.relay("B", upstream="A", relay_id="peer-test")
    h.start()

    h.case("Service config and peer listener")
    h.expect_service_listeners("A", ["public", "internal"])
    h.expect_peer_listener("A", "public")

    h.case("Publish on internal, subscribe on public")
    pub = h.actor(
        "pub", "publisher", relay="A", ns=NAMESPACE, track="date", listener="internal"
    )
    sub = h.actor(
        "sub",
        "subscriber",
        relay="A",
        ns=NAMESPACE,
        track="date",
        timeout=TIMEOUT,
        listener="public",
    )
    pub.start()
    h.wait_namespace("A", NAMESPACE)
    sub.run()
    h.expect_received(sub)
    h.expect_no_errors(sub)

    h.case("Identical rules on disjoint listeners stay separate services")
    ops_pub = h.actor(
        "ops-pub",
        "publisher",
        relay="A",
        ns=OPS_NAMESPACE,
        track="date",
        listener="ops",
    )
    ops_sub = h.actor(
        "ops-sub",
        "subscriber",
        relay="A",
        ns=OPS_NAMESPACE,
        track="date",
        timeout=TIMEOUT,
        listener="ops",
    )
    public_sub = h.actor(
        "public-sub",
        "subscriber",
        relay="A",
        ns=OPS_NAMESPACE,
        track="date",
        timeout=TIMEOUT,
        listener="public",
    )
    ops_pub.start()
    h.wait_namespace("A", OPS_NAMESPACE, service="ops")
    ops_sub.run()
    h.expect_received(ops_sub)
    public_sub.run()
    h.expect_nothing_received(public_sub)
    # Setup succeeded: "default" took the session and has no such namespace.
    h.expect_actor_output(public_sub, r"SubscribeError .*no such namespace")

    h.case("Subscribe over WebTransport on a listener with no service")
    isolated = h.actor(
        "isolated",
        "subscriber",
        relay="A",
        ns=NAMESPACE,
        track="date",
        timeout=TIMEOUT,
        listener="isolated",
    )
    mark = h.relay_log_mark("A")
    isolated.run()
    h.expect_nothing_received(isolated)
    # picoquic: a listener with no bound service gets an empty WebTransport path
    # table, so h3zero refuses the CONNECT before MOQT setup reaches moqx.
    if h.quic_stack == "picoquic":
        h.expect_actor_output(isolated, r"Non-200 response: 404")
    else:
        h.expect_relay_log("A", NO_SERVICE, since=mark)

    # mvfst only: rejecting SETUP over raw QUIC on picoquic leaks the
    # PicoConnectionContext and the session it holds.
    # https://github.com/openmoq/moxygen/issues/583
    if h.quic_stack != "picoquic":
        h.case("Subscribe over native QUIC on a listener with no service")
        isolated_quic = h.actor(
            "isolated-quic",
            "subscriber",
            relay="A",
            ns=NAMESPACE,
            track="date",
            flags="--quic_transport",
            timeout=TIMEOUT,
            listener="isolated",
        )
        mark = h.relay_log_mark("A")
        isolated_quic.run()
        h.expect_nothing_received(isolated_quic)
        h.expect_relay_log("A", NO_SERVICE, since=mark)

    ops_pub.stop()
    pub.stop()
    h.expect_relay_alive("A")
    h.expect_relay_alive("B")


main(run, base_port_key="listener_services")
