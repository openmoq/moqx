# Logging

moqx and every layer of its stack (moxygen, fizz, wangle, mvfst, picoquic transport, and over time proxygen and folly itself) emit logs through **folly's XLOG framework**. A single CLI flag controls the entire stack uniformly:

```
--logging=<config-string>
```

This document covers the config-string grammar, the category hierarchy, the severity ladder, and a generous set of practical examples for the common operational and debugging workflows.

## TL;DR

| You want… | Run this |
|---|---|
| Default quiet (just `INFO`+ from each layer) | `moqx --config c.yaml` (no logging flag — the binary baseline already does this) |
| See moqx's per-subscribe/per-cache decisions | `moqx … --logging=moqx=DBG4` |
| Trace one MoQ session class | `moqx … --logging=moxygen.MoQSession=DBG4` |
| Just QUIC, all stacks | `moqx … --logging=quic=DBG1` |
| Just picoquic, internal events | `moqx … --logging=quic.picoquic=DBG3` |
| Just mvfst | `moqx … --logging=quic.mvfst=DBG2` |
| Crank everything (firehose) | `moqx … --logging=DBG4` |
| Production: log to file, WARN baseline | `moqx … --logging=WARN --log-handler=default:async=true,sync_level=WARN` |
| Hot-path tracing without losing throughput | `moqx … --logging=moqx=DBG4 --log-handler=default:async=true,sync_level=WARN` (async on by default already) |

**Preferred form for setting the root category is the bare level:** `--logging=INFO`, `--logging=DBG4`, `--logging=WARN`. The longer `--logging=.=DBG4` is equivalent and also works.

Use **`--log-handler=…`** for handler-level settings (async/sync_level/file output/etc.). Both `--logging` and `--log-handler` can be passed multiple times — moqx combines them. This is how you avoid having to shell-quote the `;` that folly uses internally between category and handler blocks.

Everything else in this doc is "how the grammar generalizes."

## How configuration layers compose

There are three layers, applied in order:

1. **Compile-time baseline** — baked into the moqx binary via `FOLLY_INIT_LOGGING_CONFIG(...)` at build. Today's baseline is `.=INFO;default:async=true,sync_level=WARN` — root at INFO, async sink with WARN+ flushed synchronously.
2. **Environment variable** — `FOLLY_LOGGING=…` overrides the baseline if set.
3. **CLI flag** — `--logging=…` overrides everything else.

The CLI flag is what operators use day-to-day. The env var is handy in scripts and Docker. The compile-time baseline is what the binary does with no overrides at all.

## Config string grammar

A config string has up to two blocks separated by a single semicolon:

```
<categories-block> ; <handler-block>
```

The handler block is optional. **Within** each block, items are separated by **commas**:

```
CATEGORY=LEVEL,CATEGORY=LEVEL,...           # categories block
default:option=value,option=value           # handler block
```

- **`CATEGORY=LEVEL`** sets the threshold for that category and all its children. The special category `.` (single dot) is the root.
- **`default:option=value`** tunes the default log handler (async, sync_level, etc.).
- **`HANDLER_NAME=type:option=value`** in the handler block redefines a handler (e.g. send output to a file).

### Bare-level shorthand

A clause in the categories block that's just a level token (no `=`) is treated by folly as a root-category set:

```bash
--logging=DBG2                                          # equivalent to --logging=.=DBG2
--logging=WARN                                          # equivalent to --logging=.=WARN
--logging=DBG2,moxygen=DBG4                             # bare-level root plus a per-category bump
--logging=WARN --log-handler=default:async=false        # bare-level root plus handler tweak (see next section)
```

This is folly's own behavior — both forms produce identical `LogConfig`. The bare-level form is preferred for the common "set the global level" case; the explicit `.=LEVEL` form also works.

### `--logging` and `--log-handler` — two flags, no shell-quoting

folly's config string has a `;` between the categories block and the handler block. `;` is a shell command separator, so passing it in a single `--logging` value would require quoting:

```bash
moqx --logging='.=INFO;default:async=false'    # need quotes (or backslash) for ;
```

To avoid that — and to give each kind of setting its own flag — moqx splits these into two:

| Flag | What it accepts | How multiples combine |
|---|---|---|
| `--logging` | Category specs: `<category>=<level>` or a bare level (root) | Joined with `,` |
| `--log-handler` | Handler configs: `default:async=true,sync_level=WARN`, `default=file:...`, etc. | Joined with `;` |

Both can be repeated. moqx combines them into a single composite before folly sees it. No shell-quoting required for either:

```bash
moqx --logging=INFO                                   # global root level
moqx --logging=INFO --logging=moxygen=DBG4            # root + per-category
moqx --logging=INFO --log-handler=default:async=false # root + handler tweak
moqx --logging=INFO --logging=moxygen=DBG4 \
     --log-handler=default:async=true,sync_level=WARN # all three
```

The composite handed to folly for the last line is `INFO,moxygen=DBG4;default:async=true,sync_level=WARN`. You could also write that single form (with quoting), but the multi-flag style reads cleaner.

`--logging` is folly's own flag (handled natively); `--log-handler` is moqx-specific (argv preprocessor before `folly::Init`). Standalone moxygen binaries don't have `--log-handler` and require single-flag-with-quoting for the handler block.

### Delimiter rules — `;` vs `,`

| Where | Separator | Example |
|---|---|---|
| Between category specs (within categories block) | **`,`** | `moqx=DBG4,moxygen=DBG3,quic=INFO` |
| Between categories block and handler block | **`;`** (exactly one) | `.=INFO;default:async=false` |
| Between handler options (within handler block) | **`,`** | `default:async=true,sync_level=WARN` |

**No spaces around separators.** Folly tolerates whitespace internally, but the convention is `a=b,c=d;default:e=f`, not `a=b, c=d ; default:e=f`. The shim doesn't add spaces, and neither should you.

A typical multi-category config:

```
--logging=INFO,moqx=DBG4,moxygen.MoQSession=DBG3,quic.picoquic=DBG1
```

Reads as: *root level is `INFO`, but the `moqx` tree gets `DBG4`, the specific file `MoQSession` under moxygen gets `DBG3`, and the picoquic QUIC stack gets `DBG1`.*

## The category hierarchy

Each XLOG call derives its category from the source file path at compile time. The categories the moqx binary produces today (rooted at the layer they come from):

```
moqx.*                          Application layer
  moqx.MoqxRelay                  Routing/cache decisions, namespace tree
  moqx.MoqxCache                  Cache hits/misses, eviction
  moqx.UpstreamProvider           Upstream session reconnect/disconnect
  moqx.admin.*                    Admin HTTP server (metrics, state, cache-purge)
  moqx.stats.*                    Stats collectors (moq, quic, picoquic, eventbase)
  moqx.config.*                   YAML config parsing/validation
  moqx.relay.*                    TopN/CrossExec filters, ranking

moxygen.*                       Protocol layer
  moxygen.MoQSession              MoQ control + data streams, peer state
  moxygen.MoQFramer               Wire format encode/decode
  moxygen.MoQCodec                Per-stream framing
  moxygen.MoQForwarder            Track-level multiplex to subscribers
  moxygen.MoQCache                Forward-cache (object/group lookup)
  moxygen.MoQRelaySession         Relay-side session helpers
  moxygen.MoQServer               Server lifecycle, ALPN registration
  moxygen.openmoq.transport.pico  picoquic transport (C++ wrapper)

quic.*                          QUIC stacks
  quic.mvfst.*                    mvfst (Meta) — see Note A below
  quic.picoquic.*                 picoquic (alternate) — see Note A below

fizz.*                          TLS 1.3
wangle.*                        Acceptor / pipeline / SSL handlers
proxygen.*                      HTTP/3, WebTransport adapters         (in progress)
folly.*                         Async I/O, EventBase, AsyncSocket      (in progress)
```

**Note A — multi-QUIC-stack disambiguation:** moqx supports two QUIC stacks; the `quic.{mvfst,picoquic}` hierarchy lets you target either or both:

- `--logging=quic=DBG1` — both stacks at `DBG1`
- `--logging=quic.mvfst=DBG2,quic.picoquic=DBG2` — same, more explicit
- `--logging=quic.picoquic=DBG3,quic.mvfst=WARN` — picoquic verbose, mvfst quiet

**Discovering categories at runtime:** if you're not sure which category a log line came from, run with `--logging=DBG0` for a few seconds; folly prepends the category to every line.

## Severity ladder

folly's `LogLevel` enum (higher number = higher severity):

```
DBG9  …  DBG1  DBG0  →  INFO  WARN  ERR  CRITICAL  DFATAL  FATAL
 1990     1998  1999    2000  3000  4000  5000      ~∞       ~∞
```

`DBG0` is the *least* verbose debug level (just below INFO); `DBG9` is the most verbose. Higher *severity* means "more important and rarer." The convention applied across the whole stack:

| level | what it means | when you see it in steady state |
|---|---|---|
| `FATAL` / `CRITICAL` | Unrecoverable, abort (`XCHECK` failures, `XLOG(FATAL)`) | Never |
| `ERR` | Actionable error logged (something went wrong that ops should know about) | Rare |
| `WARN` | Unexpected but handled (recoverable input rejected, oversized frame, etc.) | Occasional |
| `INFO` | State transitions only — session connect/disconnect, subscribe accepted, ALPN negotiated, etc. | Per-event basis |
| `DBG1`–`DBG2` | Per-request / per-session detail (no per-frame) | When debugging |
| `DBG3`–`DBG5` | Per-packet / per-object detail | Deep tracing |
| `DBG6`–`DBG9` | Byte-level, executor tasks, queue stats, etc. | Forensic |

If you see `ERR` lines in steady state, that's a real problem — file a bug. If you see steady `WARN` lines, that's a hint of an edge case the system is handling.

## Async behavior and what `sync_level` means

The default handler is async — log writes go through a folly LoggerDB queue and a writer thread drains them. This is why per-packet `DBG3` doesn't degrade relay throughput.

But on a crash or unclean shutdown, the queue may not flush. To avoid losing critical lines, the baseline sets `sync_level=WARN`: any line at `WARN` or above is written **synchronously** before the call returns. So `ERR` / `WARN` lines from immediately before an abort are always visible; high-rate `DBG*` lines are best-effort and may be lost if the process dies before the writer drains.

If you want a different tradeoff:

```
--log-handler=default:sync_level=ERR        # only ERR+ sync, WARN goes async (faster, riskier)
--log-handler=default:sync_level=DBG0       # everything sync (slow, but every line is durable)
--log-handler=default:async=false           # turn async off entirely
```

## Practical examples

The examples below pair a scenario with the command. Severity levels are deliberate — they tell you what shape of log you'll see.

### Quiet operation (production default)

```bash
moqx --config c.yaml
```

Compile-time baseline kicks in: root at `INFO`, async on, `WARN`+ sync. Per-frame chatter is gone; you only see session-level state transitions and anything `WARN`+.

### Debugging the moqx relay layer

```bash
moqx --config c.yaml --logging=moqx=DBG4
```

Shows every routing decision, cache hit/miss, namespace-tree update, and forwarder action in `moqx.*`. Other layers stay at the compile-time `INFO` baseline.

### Trace a single source file

```bash
moqx --config c.yaml --logging=moxygen.MoQSession=DBG4
```

Just `MoQSession.cpp` lines. Useful when you're staring at one specific class.

### Trace a connection across the whole stack

```bash
moqx --config c.yaml --logging=DBG2
```

A single `INFO`-level handshake will now generate a multi-layer trace: fizz handshake → mvfst/picoquic packets → moxygen MoQ frames → moqx routing decision. Every line carries its category, so reading top-to-bottom shows the request flowing down the stack.

### Per-stack QUIC: mvfst vs picoquic

If you're running with `transport: pico` in your config and want to confirm packets are actually going through picoquic:

```bash
moqx --config c.yaml --logging=quic.picoquic=DBG3
```

You'll see per-packet `pdu` / `pkt` / `out pkt` lines from picoquic. Conversely with the mvfst transport:

```bash
moqx --config c.yaml --logging=quic.mvfst=DBG2
```

To watch both stacks at once (useful when you have a mixed deployment talking to both):

```bash
moqx --config c.yaml --logging=quic=DBG1
```

### Investigate a flaky session — combine layers

```bash
moqx --config c.yaml \
  --logging='.=INFO,moqx.MoqxRelay=DBG3,moxygen.MoQSession=DBG3,quic=DBG1'
```

Keeps overall noise at `INFO` but lights up the three layers that handle a session: routing decision, MoQ session state, QUIC packet events. A single connection life-cycle becomes traceable line by line without drowning in protocol-internal chatter.

### High-rate tracing without throughput regression

```bash
moqx --config c.yaml --logging=moxygen.MoQSession=DBG4
```

Even if `DBG4` produces 10k+ msgs/sec under load, the relay's request throughput is unaffected because the default handler is async — the cost is only the format-and-enqueue at the call site, not the actual I/O.

### Quiet down a noisy layer

```bash
moqx --config c.yaml --logging=INFO,quic=WARN
```

Useful when you don't care about QUIC chatter but want to keep app-level events. The most-specific category wins, so `quic.mvfst` and `quic.picoquic` inherit `WARN` even though they descend from `.` (which is set to `INFO`).

### Production: log to file with rotation

folly logging supports a `file:` handler for output. To redirect all output to a single file:

```bash
moqx --config c.yaml --logging=INFO \
    --log-handler=default=file:path=/var/log/moqx.log,async=true,sync_level=WARN
```

For Docker, prefer logging to stderr and letting the container runtime handle rotation:

```bash
docker run --log-driver json-file --log-opt max-size=10m --log-opt max-file=5 \
  -e FOLLY_LOGGING='.=INFO' ghcr.io/openmoq/moqx:latest
```

### Stack trace at a specific source location

(Pending — folly's `--log_backtrace_at` equivalent isn't wired into moqx today. For now: `XCHECK` and `XLOG(FATAL)` already include stack traces. To request a non-fatal stack at a specific line, file an issue.)

### Use the `FOLLY_LOGGING` env var

Useful for systemd / Docker / Kubernetes where editing the CLI line is awkward:

```bash
export FOLLY_LOGGING='.=INFO,moqx=DBG2'
moqx --config c.yaml
```

`--logging=` on the CLI wins if both are set.

## qlog (separately, for deep QUIC analysis)

XLOG and qlog are complementary, not competitors:

- **XLOG** is the operator/dev surface — uniform across the stack, async, controlled by `--logging=`.
- **qlog** is the IETF-standard structured QUIC log — JSON files for tooling like [qvis](https://qvis.quictools.info/) to analyze packet timelines, congestion control, and stream events.

For picoquic, enable qlog separately:

```bash
moqx --config c.yaml --qlog-dir /tmp/qlogs --logging=quic.picoquic=INFO
```

The XLOG channel gives you operator-level visibility (`INFO` connection lifecycle); the qlog files give you the deep-dive packet trace. Independent — one doesn't suppress the other.

## Migration from the old (glog) flags

If you have scripts that pass old flags, here's the mapping:

| Old (glog) | New (folly XLOG) |
|---|---|
| `--minloglevel 0` (INFO+) | `--logging=.=INFO` (or omit — default) |
| `--minloglevel 1` (WARNING+) | `--logging=.=WARN` |
| `--minloglevel 2` (ERROR+) | `--logging=.=ERR` |
| `-v 1` | `--logging=.=DBG1` |
| `-v 2` | `--logging=.=DBG2` |
| `-v 4` | `--logging=.=DBG4` |
| `--vmodule "MoQSession=4"` | `--logging=moxygen.MoQSession=DBG4` |
| `--vmodule "MoQ*=4"` | (no glob equivalent — list categories explicitly, or use a higher-level root like `moxygen=DBG4`) |
| `--vmodule "QuicTransportBase=2"` | `--logging=quic.mvfst.QuicTransportBase=DBG2` |
| `--logtostderr` | (default — folly writes to stderr unless a handler is configured otherwise) |
| `--log_dir DIR` | use a `file:` handler — see the production example above |
| `--stderrthreshold 1` | `--logging=INFO --log-handler=default:async=false` (or per-category sync_level) |
| `--log_backtrace_at FILE:N` | not currently equivalent (file an issue if needed) |

For during-migration scripts, glog flags from layers that haven't migrated yet (proxygen, folly itself) still work — but those layers' output is the only thing affected by them.

## Troubleshooting

**I set `--logging=foo=DBG4` and see no extra output.**
Check the category exists. Run `--logging=.=DBG0` briefly to see the actual categories being emitted; the name might be `moxygen.foo` or `moqx.foo` rather than just `foo`.

**Output is showing up but is garbled / missing newlines.**
Async logger lines can interleave with raw `printf` from native libraries that don't go through XLOG (some C deps, currently picoquic-internal until #339 finishes). Adding `--logging=INFO --log-handler=default:async=false` confirms whether this is the cause.

**I see `INFO`-level connection chatter at very high rates.**
The convention is that per-request events should be `DBG1`–`DBG2`, not `INFO`. If you spot a layer that's chattering at `INFO`, file a bug — it's a misleveled call site, not intended behavior.

**`WARN` / `ERR` lines vanish on crash.**
The baseline forces `sync_level=WARN` so this shouldn't happen, but if you override with `default:sync_level=ERR` or `async=true` without a sync level, queued lines may not flush. Restore the baseline `sync_level=WARN` and the lines come back.

**Per-file levels aren't taking effect.**
Make sure you're using the **stripped** category, not the absolute path. The build strips the source-tree prefix from `__FILE__` so `moxygen/MoQSession.cpp` becomes `moxygen.MoQSession`. Use that form; absolute paths won't match.

## Reference

- folly logging docs: see `folly/logging/docs/Overview.md`, `LogLevels.md`, `Config.md`, `LogCategories.md`, `LogHandlers.md`, `Usage.md` (in folly's own source tree).
- moqx baseline: `src/main.cpp` — search for `FOLLY_INIT_LOGGING_CONFIG`.
- Master spec for the normalization: [openmoq/moqx#339](https://github.com/openmoq/moqx/issues/339).
