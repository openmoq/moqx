# Logging

moqx and every layer of its stack (moxygen, fizz, wangle, mvfst, picoquic transport, and over time proxygen and folly itself) emit debug logs through **folly's XLOG framework**. A single CLI flag controls the entire stack uniformly:

```
--logging=<config-string>
```

## TL;DR

| You want… | Run this |
|---|---|
| Default quiet (just `INFO`+ from each layer) | `moqx --config c.yaml` (no flag — that's the baseline) |
| See moqx's per-subscribe/per-cache decisions | `moqx … --logging=moqx=DBG4` |
| Trace one MoQ session class | `moqx … --logging=moxygen.MoQSession=DBG4` |
| QUIC, all stacks | `moqx … --logging=quic=DBG1` |
| picoquic internal events | `moqx … --logging=quic.picoquic=DBG3` |
| mvfst | `moqx … --logging=quic.mvfst=DBG2` |
| Crank everything (firehose) | `moqx … --logging=DBG4` |
| Log to file instead of stderr | `moqx … --logging=INFO --log-handler=default=file:path=/var/log/moqx.log` |

The bare-level form (`--logging=INFO`, `--logging=DBG4`) is shorthand for `--logging=.=INFO`. Both work. Use `--log-handler=…` for output/async settings; both flags can be repeated and moqx combines them, so you never have to shell-quote `;`.

## Config string grammar

A config string has up to two blocks separated by a single semicolon:

```
<categories-block> ; <handler-block>
```

Inside each block, items are comma-separated:

| | Separator | Example |
|---|---|---|
| Between category specs | `,` | `moqx=DBG4,moxygen=DBG3,quic=INFO` |
| Cats block ↔ handler block | `;` (exactly one) | `.=INFO;default:async=false` |
| Between handler options | `,` | `default:async=true,sync_level=WARN` |

- `CATEGORY=LEVEL` — sets the threshold for that category and all its children. `.` (single dot) is the root.
- A bare level (no `=`) in the categories block — shorthand for `.=LEVEL`. `--logging=DBG4` ≡ `--logging=.=DBG4`.
- `default:option=value` — handler options (async, sync_level, etc.).
- `default=type:option=value` — replace the default handler (e.g. `default=file:path=/var/log/moqx.log`).

### `--logging` and `--log-handler` — two flags

Because `;` is a shell command separator, putting it in a single `--logging` value would need quoting. moqx splits the grammar into two flags so neither needs quotes, and both can be repeated:

| Flag | What it accepts | Multiples joined with |
|---|---|---|
| `--logging` | category specs (folly-native) | `,` |
| `--log-handler` | handler configs (moqx-specific) | `;` |

```bash
moqx --logging=INFO --logging=moxygen=DBG4 --log-handler=default:async=false
```

becomes the composite `INFO,moxygen=DBG4;default:async=false` before folly sees it. `--log-handler` is moqx-only; standalone moxygen binaries take the single quoted form.

## The category hierarchy

Each XLOG call derives its category from the source file path at compile time. The categories moqx produces:

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
  moxygen.MoQRelaySession         Relay-side session helpers
  moxygen.MoQServer               Server lifecycle, ALPN registration
  moxygen.relay.MoQForwarder      Track-level multiplex to subscribers
  moxygen.openmoq.transport.pico  picoquic transport (C++ wrapper)

quic.*                          QUIC stacks
  quic.mvfst.*                    mvfst (Meta)
  quic.picoquic.*                 picoquic (alternate)

fizz.*                          TLS 1.3
wangle.*                        Acceptor / pipeline / SSL handlers
proxygen.*                      HTTP/3, WebTransport adapters         (in progress)
folly.*                         Async I/O, EventBase, AsyncSocket      (in progress)
```

Both QUIC stacks live under `quic.*`. Target either or both:

```bash
--logging=quic=DBG1                                 # both
--logging=quic.picoquic=DBG3,quic.mvfst=WARN        # picoquic verbose, mvfst quiet
```

**Discovering categories at runtime:** if you're not sure which category a log line came from, run `--logging=DBG0` for a few seconds; folly prepends the category to every line.

## Severity ladder

```
DBG9  …  DBG1  DBG0  →  INFO  WARN  ERR  CRITICAL  DFATAL  FATAL
 1990     1998  1999    2000  3000  4000  5000      ~∞       ~∞
```

`DBG0` is the *least* verbose debug level (just below INFO); `DBG9` is the most verbose. The convention across the stack:

| level | what it means | steady-state frequency |
|---|---|---|
| `FATAL` / `CRITICAL` | Unrecoverable, abort (`XCHECK` failures, `XLOG(FATAL)`) | Never |
| `ERR` | Actionable error logged | Rare |
| `WARN` | Unexpected but handled | Occasional |
| `INFO` | State transitions (session connect/disconnect, subscribe accepted, ALPN negotiated) | Per-event |
| `DBG1`–`DBG2` | Per-request / per-session detail | When debugging |
| `DBG3`–`DBG5` | Per-packet / per-object detail | Deep tracing |
| `DBG6`–`DBG9` | Byte-level, executor tasks, queue stats | Forensic |

If you see `ERR` in steady state, file a bug. Steady `WARN` hints at an edge case the system is handling.

## The compile-time baseline

Set in [`src/main.cpp`](../src/main.cpp) via `FOLLY_INIT_LOGGING_CONFIG(...)`. Today's baseline:

```cpp
FOLLY_INIT_LOGGING_CONFIG(".=INFO;default:async=true,sync_level=WARN");
```

- `.=INFO` — root at INFO.
- `default:async=true` — the default handler runs asynchronously: writes go onto a queue, a background thread drains them. Per-packet `DBG3` doesn't degrade relay throughput.
- `sync_level=WARN` — lines at WARN+ bypass the queue and write synchronously, so they survive crashes. DBG lines are best-effort.

Runtime `--logging` / `--log-handler` and the `FOLLY_LOGGING` env var **merge** into this baseline (folly uses `LoggerDB::updateConfig`, not reset). So `--logging=DBG4` raises the root level but leaves `async=true,sync_level=WARN` in place. To change the handler, use `--log-handler=…`.

Different binaries can ship different defaults — edit `FOLLY_INIT_LOGGING_CONFIG` in `src/main.cpp`, or define it via `target_compile_definitions(moqx PRIVATE FOLLY_INIT_LOGGING_CONFIG=\"...\")` in CMake. The baseline doesn't strip out call sites; every `XLOG(DBG9)` is still compiled in. Separately, the **category-name derivation** *is* compile-time — `FOLLY_XLOG_STRIP_PREFIXES` in [`CMakeLists.txt`](../CMakeLists.txt) rewrites `__FILE__` so `src/MoqxRelay.cpp` becomes `moqx.MoqxRelay`.

## Common operator patterns

| Scenario | Command |
|---|---|
| Quiet (default) | `moqx --config c.yaml` |
| Trace one class | `moqx … --logging=moxygen.MoQSession=DBG4` |
| Trace a whole layer | `moqx … --logging=moqx=DBG4` |
| Trace a connection across the stack | `moqx … --logging=DBG2` |
| Mute QUIC, keep app | `moqx … --logging=INFO,quic=WARN` |
| Multi-layer session investigation | `moqx … --logging=INFO,moqx.MoqxRelay=DBG3,moxygen.MoQSession=DBG3,quic=DBG1` |
| Log to file | `moqx … --logging=INFO --log-handler=default=file:path=/var/log/moqx.log` |
| Disable async (durable, slower) | `moqx … --log-handler=default:async=false` |
| Via env var (systemd / Docker / k8s) | `FOLLY_LOGGING=.=INFO,moqx=DBG2 moqx --config c.yaml` |

CLI flags win over the env var if both are set.

## qlog (separately, for structured QUIC analysis)

XLOG and qlog have different jobs:

- **XLOG** is the ad-hoc operator/dev surface — code-embedded clues, controlled by `--logging`. Good for "what just went wrong."
- **qlog** is the IETF-standard structured QUIC log — JSON consumed by external tooling like [qvis](https://qvis.quictools.info/) for packet timelines, congestion control, stream events. Good for "trace a single session" investigations.

Per-session qlog enablement and random sampling are planned (the moqx admin surface will let you opt one connection in at a time). For now, enable qlog at the picoquic level for all connections:

```bash
moqx --config c.yaml --qlog-dir /tmp/qlogs --logging=quic.picoquic=INFO
```

Independent channels — enabling one doesn't suppress the other.

## Migration from the old (glog) flags

| Old (glog) | New (folly XLOG) |
|---|---|
| `--minloglevel 0` (INFO+) | `--logging=INFO` (or omit) |
| `--minloglevel 1` (WARNING+) | `--logging=WARN` |
| `--minloglevel 2` (ERROR+) | `--logging=ERR` |
| `-v 1` … `-v 4` | `--logging=DBG1` … `--logging=DBG4` |
| `--vmodule "MoQSession=4"` | `--logging=moxygen.MoQSession=DBG4` |
| `--vmodule "MoQ*=4"` | no glob — list categories or use a higher root (`moxygen=DBG4`) |
| `--vmodule "QuicTransportBase=2"` | `--logging=quic.mvfst.QuicTransportBase=DBG2` |
| `--logtostderr` | default (folly writes to stderr unless a handler redirects) |
| `--log_dir DIR` | `--log-handler=default=file:path=DIR/moqx.log` |
| `--stderrthreshold 1` | `--log-handler=default:async=false` (or per-category sync_level) |
| `--log_backtrace_at FILE:N` | not yet equivalent |

Layers that haven't migrated yet (proxygen, folly itself) still respect the legacy glog flags for their own output.

## Troubleshooting

**Set `--logging=foo=DBG4`, see no extra output.** Category doesn't exist by that exact name. Run `--logging=DBG0` briefly to see what folly emits; you probably want `moxygen.foo` or `moqx.foo`.

**Output garbled / missing newlines.** Async logger lines can interleave with raw `printf` from non-XLOG C deps. `--log-handler=default:async=false` confirms whether that's the cause.

**`INFO`-level chatter at very high rates.** Per-request events should be `DBG1`–`DBG2`. If a layer is chattering at `INFO`, file a bug — it's a misleveled call site.

**`WARN`/`ERR` vanish on crash.** The baseline forces `sync_level=WARN` so this shouldn't happen. If you've overridden with `default:sync_level=ERR` or `async=true` without a sync level, queued lines won't flush.

**Per-file levels not taking effect.** Use the stripped category, not the absolute path: `moxygen/MoQSession.cpp` → `moxygen.MoQSession`.

## Reference

- folly logging docs: `folly/logging/docs/` (Overview.md, LogLevels.md, Config.md, LogCategories.md, LogHandlers.md, Usage.md) in folly's source tree.
- moqx baseline: `src/main.cpp` — search for `FOLLY_INIT_LOGGING_CONFIG`.
- Master spec: [openmoq/moqx#339](https://github.com/openmoq/moqx/issues/339).
