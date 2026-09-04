#!/usr/bin/env python3
"""moq_harness.py — declarative topology harness for moqx integration tests.

Import it, declare a topology, start it, act, assert:

    from lib.moq_harness import main

    def run(h):
        h.relay("A")
        h.relay("B", upstream="A")
        h.start()
        pub = h.actor("pub", "publisher",  relay="A", ns="moq-date", track="date")
        sub = h.actor("sub", "subscriber", relay="B", ns="moq-date", track="date",
                      timeout=2)
        pub.start()
        h.wait_sessions_atleast("A", "+1")
        sub.run()
        h.expect_received(sub)

    main(run, base_port_key="chain")

Relays may form a cycle: `upstream` may name a relay declared later, and start()
brings every relay up before waiting for any of them to peer. See
docs/relay-hops.md.

Assertions are soft — they tally into Harness.failures and main() decides the
exit status, so one failure does not hide the rest of the run.

Usage: python3 test/test_relay_<name>.py [path/to/moqx] [--save-logs [DIR]]
"""

from __future__ import annotations

import argparse
import contextlib
import http.client
import json
import operator
import os
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from string import Template

from .shellvars import read_shell_vars

_LIB_DIR = Path(__file__).resolve().parent
_TEST_DIR = _LIB_DIR.parent
_REPO = _TEST_DIR.parent

READY_TIMEOUT = 15.0
POLL_INTERVAL = 0.1
# /state streams as the walk produces it, so a big topology under a sanitizer
# can take a while to finish a response that started promptly.
ADMIN_TIMEOUT = 5.0
ACTOR_GRACE = 2.0
# Longer than the relay's own 10s teardown watchdog, so a relay that trips the
# watchdog reports its _Exit(1) rather than being SIGKILLed here and blamed for
# a hang it already diagnosed.
RELAY_TERM_TIMEOUT = 15.0

# urllib honours $http_proxy and does not exempt loopback, so a proxied dev box
# or CI runner would send every admin call to the proxy and see it time out.
_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))

_OPS = {
    "ge": operator.ge,
    "gt": operator.gt,
    "eq": operator.eq,
    "lt": operator.lt,
    "le": operator.le,
}


class HarnessError(Exception):
    """A setup or usage error. Aborts the run; assertions do not."""


_ACTOR_ERROR = re.compile(r"SubscribeError|Fetch.*failed")


def _lines(path):
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []


def _tail(path, count):
    return _lines(path)[-count:]


def _dig(obj, *path):
    # obj["a"]["b"]["c"] for _dig(obj, "a", "b", "c"), but None instead of
    # raising when any level is missing or is not a dict.
    for key in path:
        if not isinstance(obj, dict) or key not in obj:
            return None
        obj = obj[key]
    return obj


def _canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _as_flags(flags):
    if flags is None:
        return []
    if isinstance(flags, str):
        return shlex.split(flags)
    return list(flags)


# "+n" is relative to the post-start() baseline, a bare n is absolute. Negative
# targets are excluded by construction: the wait is a floor, so ">= -1" is
# satisfied before it starts and would pass without waiting for anything.
_SESSION_TARGET = re.compile(r"^(\+)?([0-9]+)$")


def _session_target(context, baseline, want):
    """Resolve a session-count wait target to an absolute count."""
    if isinstance(want, int) and not isinstance(want, bool) and want >= 0:
        return want
    match = _SESSION_TARGET.match(want) if isinstance(want, str) else None
    if match is None:
        raise HarnessError(
            f'{context}: want must be a non-negative count or "+n" relative '
            f"to the baseline, not {want!r}"
        )
    value = int(match.group(2))
    return baseline + value if match.group(1) else value


def _cmp(lhs, op, rhs):
    try:
        compare = _OPS[op]
    except KeyError:
        raise HarnessError(f"unknown comparison operator: {op}") from None
    return compare(float(lhs), float(rhs))


def _reap(proc, timeout):
    """Wait for a terminating process, SIGKILLing it if it outlasts <timeout>."""
    try:
        return proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        return proc.wait()


def _until(timeout):
    """Yield once per poll attempt until <timeout> elapses.

    Pairs with for/else: break or return on success, and the else clause runs
    exactly when the wait ran out of time.
    """
    deadline = time.monotonic() + timeout
    while True:
        yield
        if time.monotonic() >= deadline:
            return
        time.sleep(POLL_INTERVAL)


def _port_free(port, socktype):
    """True if nothing holds <port> on ::.

    Replaces `ss -uln`/`ss -tln`, which does not exist on macOS or BSD — there
    the shell harness's `2>/dev/null` turned the whole check into a no-op.

    SO_REUSEPORT is deliberately never set: it only takes effect when *every*
    socket on the address sets it, so a plain bind still gets EADDRINUSE against
    an mvfst listener. SO_REUSEADDR is set for TCP only, matching what the admin
    server itself does — without it a previous run's TIME_WAIT sockets would
    read as "in use", and with it an actively listening socket still does.
    """
    try:
        sock = socket.socket(socket.AF_INET6, socktype)
    except OSError as error:
        raise HarnessError(
            f"cannot open an IPv6 socket for the port pre-flight check ({error}) "
            "— these tests need IPv6 loopback"
        ) from error
    with sock:
        with contextlib.suppress(OSError):
            sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        if socktype == socket.SOCK_STREAM:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(("::", port))
        except OSError:
            return False
        return True


# $-substitution rather than str.format: the config is YAML full of literal
# braces, which .format() would need doubled throughout.
_CONFIG_HEAD = Template(
    """\
relay_id: "$relay_id"
threads: 2
use_relay_thread: true
use_local_forwarders: true
listeners:
  - name: $name
    udp:
      socket:
        address: "::"
        port: $listen
    tls:
      insecure: true
    endpoint: "/moq-relay"
    moqt_versions: $moqt_versions
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache:
      enabled: false
      max_tracks: 100
      max_groups_per_track: 3
"""
)

_CONFIG_UPSTREAM = Template(
    """\
    upstream:
      url: "moqt://localhost:$port/moq-relay"
      tls:
        insecure: true
      idle_timeout_ms: 60000
"""
)

_CONFIG_ADMIN = Template(
    """\
admin:
  port: $port
  address: "::"
  plaintext: true
"""
)


@dataclass
class Relay:
    name: str
    listen: int
    admin: int
    upstream: str | None
    relay_id: str
    proc: subprocess.Popen | None = None
    config_path: Path | None = None
    log_path: Path | None = None
    baseline: int = 0
    killed: bool = False
    stopped: bool = False


@dataclass
class Actor:
    harness: Harness
    name: str
    kind: str
    relay: str
    ns: str
    track: str
    flags: list[str]
    timeout: float
    out_path: Path
    proc: subprocess.Popen | None = None
    deadline: float | None = None

    def argv(self):
        port = self.harness.relay_of(f"actor {self.name}", self.relay).listen
        url = f"https://localhost:{port}/moq-relay"
        moqbin = self.harness.moqbin
        if self.kind == "publisher":
            argv = [
                str(moqbin / "moqdateserver"),
                f"--relay_url={url}",
                f"--ns={self.ns}",
                "--insecure",
            ]
        else:
            argv = [
                str(moqbin / "moqtextclient"),
                f"--connect_url={url}",
                f"--track_namespace={self.ns}",
                f"--track_name={self.track}",
                "--insecure",
            ]
        return argv + self.flags

    def start(self):
        if self.proc is not None:
            raise HarnessError(f"actor_start: '{self.name}' already started")
        with open(self.out_path, "wb") as out:
            self.proc = subprocess.Popen(
                self.argv(), stdout=out, stderr=subprocess.STDOUT
            )
        # Stamped at spawn, not in wait(), so an actor started early and waited
        # on late gets the window the test declared rather than a fresh one.
        #
        # Unlike `timeout N cmd &`, nothing signals the child at N: the deadline
        # only bounds how long wait() blocks, so an actor whose wait() is
        # deferred past its deadline lives until wait() runs. No test depends on
        # the difference — they assert on what arrived, not on when it died.
        self.deadline = time.monotonic() + self.timeout if self.timeout > 0 else None

    def wait(self):
        """Wait for a started actor.

        The status is not an assertion — an actor with a timeout is expected to
        be killed at its deadline, the way `timeout` reaps one at 124. Use
        expect_received.
        """
        if self.proc is None:
            raise HarnessError(f"actor_wait: '{self.name}' was never started")
        if self.deadline is None:
            self.proc.wait()
        else:
            try:
                self.proc.wait(timeout=max(0.0, self.deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                self.proc.terminate()
                _reap(self.proc, ACTOR_GRACE)
        self.proc = None
        self.deadline = None

    def run(self):
        self.start()
        self.wait()

    def stop(self):
        if self.proc is None:
            return
        self.proc.terminate()
        _reap(self.proc, ACTOR_GRACE)
        self.proc = None
        self.deadline = None


class Harness:
    def __init__(self, binary, moqbin, base_port, tmpdir, relay_log_args):
        self.binary = binary
        self.moqbin = moqbin
        self.base_port = base_port
        self.tmpdir = tmpdir
        self.relay_log_args = list(relay_log_args)
        versions = read_shell_vars(_TEST_DIR / "test_versions.sh")
        if "MOQT_TEST_VERSIONS" not in versions:
            raise HarnessError(
                "MOQT_TEST_VERSIONS is not declared in test/test_versions.sh"
            )
        self.moqt_versions = versions["MOQT_TEST_VERSIONS"]
        self.relays: dict[str, Relay] = {}
        self.actors: dict[str, Actor] = {}
        self.failures = 0
        self.started = False
        self._port_counter = 0
        self._cleaned = False
        self._admin_error = None

    # ── Reporting ──────────────────────────────────────────────────────────────
    def _fail(self, message):
        print("FAIL: " + message, file=sys.stderr)
        self.failures += 1

    def _pass(self, message):
        print("PASS: " + message)

    def case(self, title):
        print()
        print(f"── {title} ──")

    # ── Lookup ─────────────────────────────────────────────────────────────────
    def relay_of(self, context, name):
        relay = self.relays.get(name)
        if relay is None:
            raise HarnessError(f"{context}: unknown relay '{name}'")
        return relay

    def _expected_peers(self, target):
        return sum(1 for r in self.relays.values() if r.upstream == target)

    # ── Topology declaration ───────────────────────────────────────────────────
    def relay(self, name, upstream=None, relay_id=None):
        if self.started:
            raise HarnessError(f"relay {name}: declared after start()")
        if name in self.relays:
            raise HarnessError(f"relay: duplicate relay '{name}'")
        listen = self.base_port + self._port_counter
        self._port_counter += 2
        entry = Relay(
            name=name,
            listen=listen,
            admin=listen + 1,
            upstream=upstream,
            relay_id=relay_id if relay_id else f"{name}-test",
        )
        self.relays[name] = entry
        return entry

    def actor(self, name, kind, relay, ns, track, flags=None, timeout=0):
        if name in self.actors:
            raise HarnessError(f"actor: duplicate actor '{name}'")
        if kind not in ("publisher", "subscriber"):
            raise HarnessError(f"actor {name}: kind must be publisher or subscriber")
        self.relay_of(f"actor {name}", relay)
        if not ns:
            raise HarnessError(f"actor {name}: ns required")
        # Required for publishers too, though only the subscriber's argv uses it
        # today: moqdateserver takes just --ns, but the publisher that replaces
        # it will name a full track. Declaring it now keeps that a one-line
        # change instead of a signature change across every test.
        if not track:
            raise HarnessError(f"actor {name}: track required")
        entry = Actor(
            harness=self,
            name=name,
            kind=kind,
            relay=relay,
            ns=ns,
            track=track,
            flags=_as_flags(flags),
            timeout=float(timeout),
            out_path=self.tmpdir / f"actor-{name}.out",
        )
        self.actors[name] = entry
        return entry

    # ── Startup ────────────────────────────────────────────────────────────────
    def _write_config(self, relay):
        parts = [
            _CONFIG_HEAD.substitute(
                relay_id=relay.relay_id,
                name=relay.name,
                listen=relay.listen,
                moqt_versions=self.moqt_versions,
            )
        ]
        if relay.upstream:
            upstream = self.relay_of(f"relay {relay.name}", relay.upstream)
            parts.append(_CONFIG_UPSTREAM.substitute(port=upstream.listen))
        parts.append(_CONFIG_ADMIN.substitute(port=relay.admin))
        path = self.tmpdir / f"{relay.name}.yaml"
        path.write_text("".join(parts), encoding="utf-8")
        relay.config_path = path

    def _check_ports_free(self):
        for relay in self.relays.values():
            for port, socktype, kind in (
                (relay.listen, socket.SOCK_DGRAM, "udp"),
                (relay.admin, socket.SOCK_STREAM, "tcp"),
            ):
                if not _port_free(port, socktype):
                    print(
                        f"ERROR: {kind} port {port} already in use "
                        "(stale relay process?)",
                        file=sys.stderr,
                    )
                    raise HarnessError(f"relay {relay.name}: port {port} unavailable")

    def start(self):
        """Bring the whole topology up, then wait for it to converge.

        Every relay starts before any peering wait, so a cycle works: a relay
        whose upstream is not listening yet simply reconnects with backoff.
        """
        if self.started:
            raise HarnessError("start() called twice")
        if not self.relays:
            raise HarnessError("start(): no relays declared")
        self.started = True

        for relay in self.relays.values():
            if relay.upstream is None:
                continue
            if relay.upstream not in self.relays:
                raise HarnessError(
                    f"relay {relay.name}: upstream names undeclared relay "
                    f"'{relay.upstream}'"
                )
            if relay.upstream == relay.name:
                raise HarnessError(f"relay {relay.name}: cannot be its own upstream")

        self._check_ports_free()

        for relay in self.relays.values():
            self._write_config(relay)
            # Always a real file, never devnull: the log tail is the only
            # evidence on five different failure paths, and the tmpdir is
            # deleted on exit anyway.
            relay.log_path = self.tmpdir / f"relay-{relay.name}.log"
            print(
                f"Starting relay {relay.name} "
                f"(listen {relay.listen}, admin {relay.admin})..."
            )
            argv = [str(self.binary), f"--config={relay.config_path}"]
            argv += self.relay_log_args
            with open(relay.log_path, "wb") as log:
                relay.proc = subprocess.Popen(
                    argv, stdout=log, stderr=subprocess.STDOUT
                )

        for relay in self.relays.values():
            self.wait_relay_ready(relay.name)

        for relay in self.relays.values():
            expected = self._expected_peers(relay.name)
            if expected:
                self.wait_sessions_atleast(relay.name, expected)

        for relay in self.relays.values():
            relay.baseline = self._sessions(relay.name)

        print(f"Topology up: {len(self.relays)} relay(s) converged.")

    # ── Admin plumbing ─────────────────────────────────────────────────────────
    def _admin(self, name, path):
        """GET an admin endpoint, or None with the reason in self._admin_error."""
        relay = self.relay_of("admin", name)
        url = f"http://localhost:{relay.admin}/{path}"
        try:
            with _OPENER.open(url, timeout=ADMIN_TIMEOUT) as response:
                body = response.read().decode("utf-8", "replace")
        except (OSError, http.client.HTTPException) as error:
            self._admin_error = f"{type(error).__name__}: {error}"
            return None
        self._admin_error = None
        return body

    def _state(self, name):
        body = self._admin(name, "state")
        if body is None:
            return None
        try:
            return json.loads(body)
        except ValueError:
            return None

    def _metric(self, name, metric):
        body = self._admin(name, "metrics")
        if body is None:
            return None
        prefix = f"moqx_{metric} "
        for line in body.splitlines():
            if line.startswith(prefix):
                return line[len(prefix) :].strip()
        return None

    def _sessions(self, name):
        value = self._metric(name, "moqActiveSessions")
        try:
            return int(float(value))
        except (TypeError, ValueError):
            return 0

    # ── Failure context ────────────────────────────────────────────────────────
    def _dump_relay_log(self, name):
        relay = self.relays.get(name)
        if relay is None or relay.log_path is None:
            return
        lines = _tail(relay.log_path, 40)
        if not lines:
            return
        print(f"--- relay {name} log (tail) ---", file=sys.stderr)
        print("\n".join(lines), file=sys.stderr)

    def _dump_publishers(self, ns):
        """Dump every publisher on that namespace.

        The dominant failure is "the subscriber got nothing because the
        publisher never announced", so the publisher's output is the evidence
        that matters.
        """
        for actor in self.actors.values():
            if actor.kind != "publisher":
                continue
            if ns and actor.ns != ns:
                continue
            lines = _tail(actor.out_path, 20)
            if not lines:
                continue
            print(
                f"--- publisher {actor.name} (ns={actor.ns}) output ---",
                file=sys.stderr,
            )
            print("\n".join(lines), file=sys.stderr)

    # ── Wait primitives ────────────────────────────────────────────────────────
    def wait_relay_ready(self, name):
        relay = self.relay_of("wait_relay_ready", name)
        for _ in _until(READY_TIMEOUT):
            if self._admin(name, "info") is not None:
                break
            # A dead relay can still "pass" this check if something else is
            # listening on its admin port — another test sharing the port block,
            # or a stale process. Fail on the corpse rather than converge
            # against a stranger.
            if relay.proc is not None and relay.proc.poll() is not None:
                self._dump_relay_log(name)
                raise HarnessError(
                    f"relay {name} died during startup "
                    f"(port {relay.listen} already in use?)"
                )
        else:
            self._dump_relay_log(name)
            raise HarnessError(
                f"relay {name} not ready after {READY_TIMEOUT:g}s ({self._admin_error})"
            )
        if relay.proc is not None and relay.proc.poll() is not None:
            raise HarnessError(
                f"relay {name} is not running, but its admin port answered "
                "— port conflict"
            )

    def wait_sessions_atleast(self, name, want):
        """Wait for moqActiveSessions >= want.

        The relative form ("+n") is measured against the post-start() baseline,
        which is what a test means by "one more session than the settled
        topology".
        """
        relay = self.relay_of("wait_sessions_atleast", name)
        want = _session_target(f"wait_sessions_atleast {name}", relay.baseline, want)
        value = 0
        for _ in _until(READY_TIMEOUT):
            value = self._sessions(name)
            if value >= want:
                return
        self._dump_relay_log(name)
        raise HarnessError(
            f"relay {name}: moqActiveSessions={value} < {want} after {READY_TIMEOUT:g}s"
        )

    def wait_namespace(self, name, ns):
        self.relay_of("wait_namespace", name)
        for _ in _until(READY_TIMEOUT):
            children = _dig(
                self._state(name), "services", "default", "namespace_tree", "children"
            )
            if isinstance(children, dict) and ns in children:
                return
        body = self._admin(name, "state")
        if body is not None:
            print(body, file=sys.stderr)
        self._dump_relay_log(name)
        raise HarnessError(
            f"namespace '{ns}' did not reach relay {name} after {READY_TIMEOUT:g}s"
        )

    # ── Relay lifecycle ────────────────────────────────────────────────────────
    def relay_kill(self, name):
        """SIGKILL a relay and flag it, exempting it from the clean-exit check.

        Death by signal is expected and not checked, so this asserts nothing —
        assert on the survivors instead.

        Peers do not observe this until idle_timeout_ms, so nothing downstream
        of it is assertable within a test. Use relay_stop when the test needs
        the peers to react.
        """
        relay = self.relay_of("relay_kill", name)
        if relay.proc is None or relay.killed or relay.stopped:
            return
        relay.proc.kill()
        relay.proc.wait()
        relay.killed = True

    def relay_stop(self, name):
        """SIGTERM a relay, wait for it, and assert it exited cleanly.

        The graceful counterpart to relay_kill: the relay closes its sessions on
        the way out, so peers notice at once rather than waiting out
        idle_timeout_ms. The exit status is the assertion — a clean shutdown
        path is the thing being tested.
        """
        relay = self.relay_of("relay_stop", name)
        if relay.proc is None or relay.killed or relay.stopped:
            return
        relay.proc.terminate()
        try:
            status = relay.proc.wait(timeout=RELAY_TERM_TIMEOUT)
        except subprocess.TimeoutExpired:
            # Not routed through _reap: it folds a hang into the same -9 a crash
            # would produce, and "hung in teardown" is the whole finding.
            relay.proc.kill()
            relay.proc.wait()
            relay.stopped = True
            self._fail(
                f"[{name}] relay did not exit within {RELAY_TERM_TIMEOUT:g}s of "
                "SIGTERM — hung in teardown"
            )
            self._dump_relay_log(name)
            return
        relay.stopped = True
        if status == 0:
            self._pass(f"[{name}] relay exited cleanly on SIGTERM")
        else:
            self._fail(
                f"[{name}] relay exited {status} on SIGTERM — crash or sanitizer leak"
            )
            self._dump_relay_log(name)

    # ── Assertions ─────────────────────────────────────────────────────────────
    def expect_received(self, actor):
        out = _lines(actor.out_path)
        data = [line for line in out if line[:1] in "0123456789"]
        if data:
            self._pass(f"[{actor.name}] received: {data[0]}")
        else:
            self._fail(f"[{actor.name}] no data received")
            print("\n".join(out), file=sys.stderr)
            self._dump_publishers(actor.ns)
            self._dump_relay_log(actor.relay)

    def expect_no_errors(self, actor):
        bad = [line for line in _lines(actor.out_path) if _ACTOR_ERROR.search(line)]
        if bad:
            self._fail(f"[{actor.name}] error in output")
            print("\n".join(bad), file=sys.stderr)
            self._dump_publishers(actor.ns)
            self._dump_relay_log(actor.relay)
        else:
            self._pass(f"[{actor.name}] no subscribe/fetch errors")

    def _fetch_state(self, context, name):
        """Return (state, services.default) for an assertion, or (None, None).

        Records the failure itself, so a caller that gets None just returns.
        """
        self.relay_of(context, name)
        body = self._admin(name, "state")
        if body is None:
            self._fail(f"[{name} /state] request failed: {self._admin_error}")
            return None, None
        try:
            state = json.loads(body)
        except ValueError:
            self._fail(f"[{name} /state] not valid JSON")
            print(body, file=sys.stderr)
            return None, None
        return state, _dig(state, "services", "default")

    def expect_namespace_present(self, name, ns):
        # Bails on state, not service: a /state without services.default is a
        # finding to report, not a reason to skip the assertion.
        state, service = self._fetch_state("expect_namespace_present", name)
        if state is None:
            return
        children = _dig(service, "namespace_tree", "children")
        if isinstance(children, dict) and ns in children:
            self._pass(f"[{name} /state] namespace_tree contains {ns}")
        else:
            self._fail(f"[{name} /state] namespace_tree missing {ns}")

    def expect_peer_count(self, name, op, want):
        state, service = self._fetch_state("expect_peer_count", name)
        if state is None:
            return
        peers = _dig(service, "downstream_peers")
        got = len(peers) if isinstance(peers, list) else 0
        if _cmp(got, op, want):
            self._pass(f"[{name} /state] downstream_peers={got} ({op} {want})")
        else:
            self._fail(f"[{name} /state] downstream_peers={got}, want {op} {want}")

    def expect_upstream_state(self, name, want):
        state, service = self._fetch_state("expect_upstream_state", name)
        if state is None:
            return
        got = _dig(service, "upstream", "state")
        got = got if got is not None else "none"
        if got == want:
            self._pass(f"[{name} /state] upstream.state={got}")
        else:
            self._fail(f"[{name} /state] upstream.state={got}, want {want}")

    def expect_relay_id(self, name, want):
        state, _ = self._fetch_state("expect_relay_id", name)
        if state is None:
            return
        got = _dig(state, "relay_id")
        if got == want:
            self._pass(f"[{name} /state] relay_id={got}")
        else:
            self._fail(f"[{name} /state] relay_id={got}, want {want}")

    def expect_metric(self, name, metric, op, want):
        self.relay_of("expect_metric", name)
        got = self._metric(name, metric)
        if got is None:
            if self._admin_error:
                self._fail(f"[{name} /metrics] request failed: {self._admin_error}")
            else:
                self._fail(f"[{name} /metrics] moqx_{metric} not present")
            return
        if _cmp(got, op, want):
            self._pass(f"[{name} /metrics] moqx_{metric}={got} ({op} {want})")
        else:
            self._fail(f"[{name} /metrics] moqx_{metric}={got}, want {op} {want}")

    def expect_relay_alive(self, name):
        relay = self.relay_of("expect_relay_alive", name)
        if relay.killed or relay.stopped:
            self._fail(f"[{name}] relay was shut down by the test")
        elif relay.proc is not None and relay.proc.poll() is None:
            self._pass(f"[{name}] relay alive")
        else:
            self._fail(f"[{name}] relay is not running")
            self._dump_relay_log(name)

    def expect_settled(self, settle, *names):
        """Snapshot each relay's whole namespace_tree, settle, re-snapshot, compare.

        This is the observable for relay-hops loop suppression: /state exposes
        no hop_id or hop_path, so a tree that stops changing is the available
        signal. The comparison spans the entire tree, not one namespace —
        circulation shows up as churn anywhere in it.

        Defaults to every relay still up, which is what the assertion means: an
        unlisted relay is exactly where circulation would hide. Relays the test
        killed or stopped are excluded rather than failing on an admin port
        that is gone.
        """
        if not names:
            names = [n for n, r in self.relays.items() if not (r.killed or r.stopped)]
        if not names:
            raise HarnessError("expect_settled: no relays still up")
        before = {}
        for name in names:
            self.relay_of("expect_settled", name)
            state = self._state(name)
            if state is None:
                self._fail(
                    f"[{name}] /state unreadable for the stability baseline: "
                    f"{self._admin_error}"
                )
                self._dump_relay_log(name)
                continue
            before[name] = _canonical(
                _dig(state, "services", "default", "namespace_tree")
            )

        time.sleep(settle)

        for name, baseline in before.items():
            state = self._state(name)
            if state is None:
                self._fail(
                    f"[{name}] /state unreadable for the stability re-check: "
                    f"{self._admin_error}"
                )
                self._dump_relay_log(name)
                continue
            after = _canonical(_dig(state, "services", "default", "namespace_tree"))
            if after == baseline:
                self._pass(f"[{name}] namespace_tree stable over {settle:g}s")
            else:
                self._fail(f"[{name}] namespace_tree changed over {settle:g}s")
                print("  before: " + baseline, file=sys.stderr)
                print("  after:  " + after, file=sys.stderr)

    # ── Teardown ───────────────────────────────────────────────────────────────
    def cleanup(self):
        """Stop everything. Returns True if any relay exited badly."""
        if self._cleaned:
            return False
        self._cleaned = True

        actors = [a for a in self.actors.values() if a.proc is not None]
        relays = [
            r
            for r in self.relays.values()
            if r.proc is not None and not r.killed and not r.stopped
        ]

        for proc in [a.proc for a in actors] + [r.proc for r in relays]:
            with contextlib.suppress(OSError):
                proc.terminate()

        # Actors get a short grace, then SIGKILL. Their status is not
        # meaningful — most are killed at their deadline by design.
        deadline = time.monotonic() + ACTOR_GRACE
        for actor in actors:
            _reap(actor.proc, max(0.0, deadline - time.monotonic()))

        relay_failed = False
        for relay in relays:
            status = _reap(relay.proc, RELAY_TERM_TIMEOUT)
            if status != 0:
                print(
                    f"FAIL: relay {relay.name} exited {status} — crash, hang, "
                    "or sanitizer leak",
                    file=sys.stderr,
                )
                relay_failed = True

        return relay_failed


def _resolve_moqbin(binary):
    """Resolve the moxygen install's bin/ (moqdateserver, moqtextclient).

    An MOQBIN already in the environment wins (ctest sets it per test);
    otherwise it comes from the tool-paths file the configure writes next to
    the binary. Mirrored in test/test_moqbin.sh for the bash tests.
    """
    from_env = os.environ.get("MOQBIN")
    if from_env:
        return Path(from_env)
    tools_env = Path(binary).resolve().parent / "moqx-tools.env"
    if tools_env.is_file():
        return Path(read_shell_vars(tools_env).get("MOQBIN", ""))
    return Path("")


def _resolve_base_port(key):
    """Resolve this test's port base.

    Every harness test must claim its own base in test_ports.sh: ctest runs
    these in parallel, and a shared base means concurrent tests collide.
    """
    name = f"TEST_HARNESS_{key.upper()}_BASE"
    from_env = os.environ.get("HARNESS_BASE_PORT")
    if from_env:
        raw, source = from_env, "HARNESS_BASE_PORT"
    else:
        ports = read_shell_vars(_TEST_DIR / "test_ports.sh")
        if name not in ports:
            raise HarnessError(
                f"{name} is not declared in test/test_ports.sh — claim a base "
                "there before running this test"
            )
        raw, source = ports[name], f"{name} in test/test_ports.sh"
    try:
        return int(raw)
    except ValueError:
        raise HarnessError(f"{source} is not a number: {raw!r}") from None


def _parse_args(argv):
    parser = argparse.ArgumentParser(description="moqx relay topology integration test")
    parser.add_argument(
        "binary", nargs="?", default=None, help="path to the moqx binary"
    )
    parser.add_argument(
        "--save-logs",
        nargs="?",
        const=True,
        default=None,
        metavar="DIR",
        help="save relay DBG4 logs, configs and actor output to DIR",
    )
    # ctest passes the binary positionally; a bare "" from a shell caller is
    # not a path.
    return parser.parse_args([a for a in argv if a != ""])


def main(run, base_port_key):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    sys.stderr.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)

    args = _parse_args(sys.argv[1:])
    script_stem = Path(sys.argv[0]).stem

    # ctest's TIMEOUT sends SIGTERM, and Python's default handler exits without
    # running `finally` — leaving relays bound to this test's ports and failing
    # every later run. Restoring the default handler first means a second signal
    # is fatal, so cleanup cannot be re-entered.
    def on_signal(signum, _frame):
        signal.signal(signal.SIGTERM, signal.SIG_DFL)
        signal.signal(signal.SIGINT, signal.SIG_DFL)
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    harness = None
    tmpdir = None
    log_dir = None
    relay_failed = False
    rc = 0
    try:
        binary = Path(args.binary) if args.binary else _REPO / "build/default/moqx"
        moqbin = _resolve_moqbin(binary)
        base_port = _resolve_base_port(base_port_key)

        relay_log_args = []
        if args.save_logs is not None:
            # Per-test subdirectory: concurrent tests all produce relay-A.log
            # and A.yaml, and would otherwise clobber each other under
            # ctest --parallel.
            log_dir = (
                Path(args.save_logs)
                if isinstance(args.save_logs, str)
                else _REPO / ".scratch/moq_harness_logs" / script_stem
            )
            # Deliberately NOT applied to actors: expect_received looks for a
            # leading digit, and a DBG4 line starting with one would false-PASS.
            relay_log_args = ["--logging=DBG4"]

        for path in (binary, moqbin / "moqdateserver", moqbin / "moqtextclient"):
            if not os.access(path, os.X_OK):
                raise HarnessError(f"not found or not executable: {path}")

        # Scratch under the repo's .scratch (a gitignored symlink) rather than
        # /tmp: DBG4 relay logs get large and /tmp is tmpfs on some hosts.
        scratch = _REPO / ".scratch"
        tmpdir = Path(
            tempfile.mkdtemp(
                prefix="moq_harness.", dir=scratch if scratch.is_dir() else None
            )
        )

        harness = Harness(binary, moqbin, base_port, tmpdir, relay_log_args)
        run(harness)
    except HarnessError as error:
        print(f"HARNESS ERROR: {error}", file=sys.stderr)
        rc = 1
    except SystemExit as error:
        rc = error.code if isinstance(error.code, int) else 1
    finally:
        relay_failed = harness.cleanup() if harness is not None else False
        if tmpdir is not None:
            if log_dir is not None:
                log_dir.mkdir(parents=True, exist_ok=True)
                for pattern in ("*.log", "*.out", "*.yaml"):
                    for path in tmpdir.glob(pattern):
                        shutil.copy2(path, log_dir / path.name)
                print(f"Logs saved to {log_dir}")
            shutil.rmtree(tmpdir, ignore_errors=True)

    if rc == 0:
        print()
        if harness is not None and harness.failures:
            print(f"{harness.failures} assertion(s) failed.", file=sys.stderr)
            rc = 1
        elif relay_failed:
            print("All assertions passed, but a relay exited badly.", file=sys.stderr)
            rc = 1
        else:
            print("All assertions passed.")
    sys.exit(rc)
