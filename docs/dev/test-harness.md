# Writing a Relay Topology Test

`test/lib/moq_harness.py` runs multi-relay integration tests: it generates a
config per relay, starts the processes, waits for the topology to converge,
runs publisher/subscriber actors against it, and asserts on the admin
endpoints. A test is a Python file that declares a topology and acts on it —
no process management, no port arithmetic, no `curl | jq`.

The existing tests are the best worked examples:

| Test | Shape | What it shows |
|---|---|---|
| [`test/test_relay_chain.py`](/test/test_relay_chain.py) | A ← B | Both directions, joining fetch, `--publish` push mode |
| [`test/test_relay_pyramid.py`](/test/test_relay_pyramid.py) | tree | Multi-hop fan-out, killing a relay, containment |
| [`test/test_relay_hops_cycle.py`](/test/test_relay_hops_cycle.py) | A → B → C → A | A cycle, and asserting on stability rather than an event |
| [`test/test_listener_services.py`](/test/test_listener_services.py) | A ← B, four listeners on A | Services bound to some of a relay's listeners, identical rules on disjoint listeners |

## The shape of a test

```python
#!/usr/bin/env python3
"""test_relay_test_name.py — one line on what this proves.

Usage: python3 test/test_relay_test_name.py [path/to/moqx] [--save-logs [DIR]]
"""

from lib.moq_harness import main

NAMESPACE = "moq-date"


def run(h):
    h.relay("A")
    h.relay("B", upstream="A")
    h.start()

    h.case("Data published at A is served by B")

    pub = h.actor("pub", "publisher", relay="A", ns=NAMESPACE, track="date")
    sub = h.actor("sub", "subscriber", relay="B", ns=NAMESPACE, track="date", timeout=2)

    pub.start()
    h.wait_sessions_atleast("A", "+1")
    sub.run()

    h.expect_received(sub)
    h.expect_no_errors(sub)

    pub.stop()


main(run, base_port_key="test_name")
```

`main()` handles argument parsing, binary resolution, the scratch directory,
signal handling, teardown and the exit status. `run(h)` is the whole test.

The `from lib.moq_harness import ...` import works because the script lives in
`test/`, which Python puts on `sys.path` as the script directory. Run the file
directly — do not run it from inside `test/lib/`.

## Four steps to a new test

### 1. Claim a port base

Every harness test gets its own base in
[`test/test_ports.sh`](/test/test_ports.sh), because `scripts/test.sh` runs
`ctest --parallel` and two tests sharing a base will fight over the same
sockets — badly enough that one relay's readiness check can pass against
another test's relay:

```bash
TEST_HARNESS_TEST_NAME_BASE=19870
```

The variable name is derived from `base_port_key`: `test_name` becomes
`TEST_HARNESS_TEST_NAME_BASE`. The harness raises a `HarnessError` naming the
missing variable if you forget.

Ten ports are reserved per test. `h.relay()` hands out one port per listener
plus one for admin, counting up from the base, so five one-listener relays fill
the block. A topology that needs more raises a `HarnessError`. The block from
`19800` is reserved for harness tests through `19879`. Do not write literal
port numbers in comments in that file — the duplicate check greps every number
in it.

### 2. Write the test file

`test/test_relay_<name>.py`, following the skeleton above. Keep it Python
3.9-compatible (that is the floor CMake asserts, set by the macOS Command Line
Tools) — `from __future__ import annotations` if you want modern annotations.

### 3. Register it with ctest

In [`test/CMakeLists.txt`](/test/CMakeLists.txt):

```cmake
add_test(
  NAME relay_test_name
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/test/test_relay_test_name.py $<TARGET_FILE:moqx>
)
set_tests_properties(relay_test_name PROPERTIES
  TIMEOUT 120
  ENVIRONMENT "MOQBIN=${MOXYGEN_BIN_DIR}"
)
```

`MOQBIN` is where `moqdateserver` and `moqtextclient` come from. Without it the
harness falls back to the `moqx-tools.env` written next to the binary at
configure time, which is what makes a direct `python3 test/...` run work.

Pick the `TIMEOUT` from the wall-clock cost under a sanitizer, not from a clean
build: the topology startup alone can take several seconds per relay.

### 4. Run it

```bash
ctest --test-dir build/san --output-on-failure -R '^relay_test_name$'
```

or directly, which is faster to iterate on:

```bash
python3 test/test_relay_test_name.py build/san/moqx
python3 test/test_relay_test_name.py build/san/moqx --save-logs
```

`--save-logs` runs the relays at `DBG4` and copies their logs, the generated
configs and every actor's output to `.scratch/moq_harness_logs/<test name>/`
(or to a directory you name). A picoquic run appends `_picoquic` to the test
name. A failed run saves the same files there without `--save-logs`, at the
default log level. Otherwise the scratch directory is deleted on exit. A relay
that exits non-zero also prints its sanitizer report, or its log tail if it
has none. The debug logging is deliberately *not* applied to actors —
`expect_received` looks for a leading digit, and a `DBG4` line starting with
one would false-PASS.

## Declaring the topology

```python
h.relay(
    name,
    *,
    upstream=None,
    upstreams=None,
    relay_id=None,
    listeners=None,
    service_listeners=None,
    extra_services=None,
)
```

`upstream` names another relay by its harness name; the generated config points
this relay's upstream URL at that relay's first listener. It may name a relay
declared *later*, so cycles work: `start()` brings every relay up before
waiting for any of them to peer, and a relay whose upstream is not listening
yet just reconnects with backoff. `upstreams` takes a list of such names.
`relay_id` defaults to `<name>-test` and is the operational identity reported
by `/state`.

`listeners` names the relay's listeners; the default is one, named after the
relay. Actors that name no listener connect to the first. `service_listeners`
binds the default service to those listeners only; omitted, the service stays
on all of them. `extra_services` maps further service names to the listeners
each is bound to; every one gets the default service's match rule, so two
services on one listener raise a `HarnessError`.

```python
h.start()
```

Validates the topology, pre-flights every port, writes the configs, starts all
relays, waits for each admin endpoint, then waits for each relay to accept the
number of peer sessions the topology implies, and to list them as
`downstream_peers` — no hand-counted session numbers.
Finally it records each relay's session count as its **baseline**, which is
what `"+n"` is measured against later. Declaring a relay after `start()` is an
error; declaring an actor after it is normal.

The generated config is fixed: two threads, relay thread and local forwarders
on, cache disabled, insecure TLS, endpoint `/moq-relay`, and `moqt_versions`
read from [`test/test_versions.sh`](/test/test_versions.sh). A test that needs
a different relay configuration needs a new knob on `h.relay()` and a matching
`Template` fragment in the harness — that is the intended way to extend it,
rather than writing a config file beside the test.

## Actors

```python
actor = h.actor(name, kind, relay, ns, track, flags=None, timeout=0, listener=None)
```

`kind` is `"publisher"` (`moqdateserver`) or `"subscriber"`
(`moqtextclient`). `flags` is extra argv — a string is `shlex`-split, a list is
taken as-is (`flags="--jrfetch --join_start=1"`). `timeout` bounds how long
`wait()` blocks; `0` means wait forever, which is what a long-lived publisher
wants. `track` is required for both kinds but only reaches the command line for
subscribers; on a publisher it is declaration only. `listener` picks one of the
relay's listeners; the default is its first.

| Call | Effect |
|---|---|
| `actor.start()` | Spawn it and stamp its deadline |
| `actor.wait()` | Block until it exits or hits its deadline, then `SIGTERM` |
| `actor.run()` | `start()` then `wait()` — the usual call for a subscriber |
| `actor.stop()` | `SIGTERM` now, for a publisher you are done with |

The deadline is stamped at `start()`, not at `wait()`, so an actor started
early and waited on late gets the window the test declared. A subscriber that
has not logged that it is running when `wait()` starts gets a new window of
`timeout` from when it does. `wait()` polls up to 5s (`STARTUP_TIMEOUT`) for
that log line, so budget a failing subscriber that much extra time. Nothing
signals the child at the deadline itself: it lives until `wait()` runs. An
actor with a timeout is *expected* to be killed at it, so assert on
`expect_received`. `expect_exited` is for an actor that should end by itself,
such as a client the relay rejects.

Output goes to `actor-<name>.out` in the scratch directory, which is what the
assertions read and what failure dumps print.

## Waiting

A test should use these waits rather than `time.sleep`:

| Call | Waits for |
|---|---|
| `h.wait_relay_ready(name)` | The admin endpoint answers (`start()` already does this) |
| `h.wait_sessions_atleast(name, want)` | `moqActiveSessions >= want` — see below |
| `h.wait_namespace(name, ns, service="default")` | `ns` appears in that service's `namespace_tree` on that relay |
| `h.wait_relay_log(name, text)` | A line of that relay's log, since it started, contains `text` |
| `h.wait_namespace_route(name, ns, via)` | That relay's best route for `ns` runs through relay `via` |
| `h.wait_routes(name, ns, want)` | That relay holds exactly `want` routes for `ns` |
| `h.wait_subscription(name, ns, track)` | That relay's `subscriptions` include `ns`/`track` |
| `h.wait_peer_count(name, want)` | That relay lists exactly `want` `downstream_peers` (`start()` already does this) |

Each raises `HarnessError` on timeout after dumping the relay log, aborting the
run rather than tallying a failure, because everything after it is meaningless.

`wait_sessions_atleast` takes exactly two forms: `"+n"`, relative to the
post-`start()` baseline, or a bare non-negative count (`3` or `"3"`), absolute.
Anything else — a negative number, `"-n"`, `"3.0"`, a float — raises a
`HarnessError` naming the bad value. The name says `atleast` because the
comparison is a floor, and the floor is the whole semantics.

Three things to keep in mind:

- A relay counts its **inbound** sessions. Its own outbound upstream link does
  not count, so a leaf relay with one upstream and no subscribers sits at zero.
- **There is no way to wait for a session count to fall.** A floor can only
  wait for a rise, and a relative decrease would be vacuous — `>= baseline-1`
  is already true before anything tears down. That is why `"-n"` is rejected
  outright rather than accepted and quietly ignored.
- To sequence a test after a teardown, keep another actor alive and wait for an
  *increase* past it. Relay-side teardown is async, so a wait that could be
  satisfied by the departing session is not a signal at all.
  `test_relay_chain.py` does exactly this in direction 4.
- `wait_namespace` returns as soon as the namespace *appears*, which can be
  mid-convergence. If what follows measures stability, sleep briefly first —
  otherwise you compare two points on the same settling curve and report benign
  convergence as instability.

A `time.sleep` is legitimate for the settle window itself; it is not a
substitute for a wait.

## Assertions

**`expect_*` means "this is true right now."** Every assertion takes a single
sample and reports on it — none of them retry, and none of them take a timeout.
Waiting is the job of the `wait_*` primitives, which is why the two families
are named differently. Keep that split when adding to either: a new assertion
samples once, and anything that needs to tolerate a delay belongs in a
`wait_*` with an explicit timeout.

The practical consequence is that an assertion placed immediately after the
event it measures can be racing that event. Sequence it with a `wait_*` first,
and reach for a bare `time.sleep` only where no wait can express it.

Assertions are **soft**: each one prints `PASS:` or `FAIL:` and tallies into
`h.failures`, and `main()` turns a non-zero tally into exit 1. One failure does
not hide the rest of the run. Failures dump their own evidence — the relay log
tail, the actor's output, the publishers on that namespace.

| Assertion | Checks |
|---|---|
| `expect_received(actor)` | The actor's output has a data line |
| `expect_nothing_received(actor)` | The actor's output has no data line |
| `expect_actor_output(actor, pattern)` | A line of the actor's output matches the regex |
| `expect_exited(actor)` | The actor ended on its own: neither `wait()` nor `stop()` terminated it |
| `expect_no_errors(actor)` | No `SubscribeError` / failed fetch in its output |
| `expect_relay_log(name, pattern, since=0)` | A line of the relay's log after `since` matches the regex; reliable for `WARN` and above, which the relay writes synchronously. Take `since` from `h.relay_log_mark(name)` before the action under test |
| `expect_namespace_present(name, ns)` | `/state` `namespace_tree` contains `ns` |
| `expect_peer_count(name, op, want)` | `/state` `downstream_peers` length |
| `expect_peer_listener(name, want)` | A `/state` downstream peer connected on listener `want` |
| `expect_service_listeners(name, want)` | `/config` `services.default.listeners` equals `want` |
| `expect_upstream_state(name, want)` | `/state` `upstream.state`, e.g. `"connected"` |
| `expect_relay_id(name, want)` | `/state` `relay_id` |
| `expect_metric(name, metric, op, want)` | A `moqx_*` Prometheus metric |
| `expect_relay_alive(name)` | The relay process is still running |
| `relay_stop(name)` | Doubles as an assertion: the relay exited 0 on `SIGTERM` |
| `expect_settled(settle, *names)` | Every named relay's whole `namespace_tree` is unchanged over a `settle`-second window; defaults to all relays still up |

`op` is one of `ge`, `gt`, `eq`, `lt`, `le`; both sides are compared as floats.

`h.case("title")` prints a section header. Use one per scenario — it is what
makes a failing run readable.

Assert on topology, not on schema. The shape of `/state` — field types,
omission rules, nesting — is covered directly at the serializer level by
`StateStreamTest` and `StateResponseTest` in `moqx_admin_stats_test`, which is
both stronger and cheaper than checking it through a live relay.

## Taking a relay down

Two ways, testing two different things. Pick by what you want to assert.

```python
h.relay_kill("B")  # SIGKILL — abrupt loss, asserts nothing
h.relay_stop("B")  # SIGTERM — graceful shutdown, asserts a clean exit
```

**`relay_kill`** models a relay vanishing. Death by signal is expected, so the
exit status is not checked and the relay is exempt from the end-of-run
clean-exit check. It asserts nothing at all — **assert on the survivors**. The
reason is what a kill does *not* do: a peer does not notice a `SIGKILL`ed relay
until `idle_timeout_ms` (60s in the generated config), well past the end of any
test. Even the survivor's `downstream_peers` is vacuous right after a kill.
Assert on behavior — that a new subscriber on the surviving branch still gets
data, which is what `test_relay_pyramid.py` does.

Do not follow a kill with an assertion that the relay is dead. The harness
reaped it inside `relay_kill`; any such check would only be reading back a flag
the same line just set.

**`relay_stop`** models an operator restarting a relay. It `SIGTERM`s, waits up
to 15s, and the exit status *is* the assertion. Three distinct failures surface
here: a non-zero exit (a crash on the shutdown path, or a sanitizer leak
report), and separately a process that never exits at all, reported as "hung in
teardown" rather than as the `-9` the follow-up `SIGKILL` produces. Because the
relay closes its sessions on the way out, the peer reacts immediately — this is
what makes `expect_peer_count("A", "eq", 0)` a real assertion a second later.

Note the limit of what it proves: the *process* exited cleanly. That peers were
notified properly is a separate claim, and the assertion you write after it.

A test does not need to take its relays down at the end. Teardown `SIGTERM`s
every relay the test left running and fails the run if one exits non-zero —
`relay_stop` is for when you need that check to happen *mid-test*, so the
assertions that follow can observe the aftermath.

`h.relay_signal(name, signum)` sends a running relay any other signal and
asserts nothing. [`test_relay_sighup.py`](/test/test_relay_sighup.py) follows a
`SIGHUP` with assertions that the relay and its sessions kept running.

## Failing correctly

Two failure modes, deliberately distinct:

- **`HarnessError`** — setup or usage is wrong (unknown relay, duplicate actor,
  port in use, a wait that timed out). Aborts the run, prints
  `HARNESS ERROR: ...`, exit 1.
- **A soft failure** — the system under test behaved wrong. Tallied, run
  continues.

Raise `HarnessError` from new harness code for anything a test *author* got
wrong; use `_fail()` for anything the *relay* got wrong.

## Extending the harness

Add to `test/lib/moq_harness.py` rather than working around it in a test. The
things most likely to need it:

- **A new relay config knob** — a keyword on `h.relay()`, a `Template` fragment,
  a line in `_write_config`.
- **A new actor kind** — a branch in `Actor.argv`. Note that `expect_received`'s
  "a line starting with a digit" rule is a `moqtextclient` convention; a new
  kind may need its own assertion.
- **A new `/state` assertion** — go through `_fetch_state`, which reports a
  failed or malformed response itself so the caller can just return.

Three shell files stay the source of truth and are parsed, not duplicated:
`test/test_ports.sh`, `test/test_versions.sh`, and the generated
`moqx-tools.env` ([`test/lib/shellvars.py`](/test/lib/shellvars.py)). The
process-reaping policy in `Harness.cleanup()` mirrors
[`test/test_relay_lifecycle.sh`](/test/test_relay_lifecycle.sh), and
`_resolve_moqbin` mirrors [`test/test_moqbin.sh`](/test/test_moqbin.sh); those
pairs must stay in agreement while both the bash and Python tests exist.
