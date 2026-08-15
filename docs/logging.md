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
| Log to file instead of stderr | redirect stderr at the shell / systemd / container layer (see "File output" below) |

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
- `default=type:option=value` — replace the default handler. Note: only the `stream` type is auto-registered today; folly's `file` type exists but is intentionally NOT registered (security: a config string could otherwise write to arbitrary paths). Use shell/systemd/container redirection for file output instead.

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

### Environment variables (no command line)

When you can't pass flags — a container image's fixed `ENTRYPOINT`, a systemd
unit, a `docker run` — set the level via the environment instead:

| Var | Owner | Notes |
|---|---|---|
| `MOQX_LOGGING` | moqx | The knob to reach for. Same grammar as `--logging` (`DBG2`, `INFO,quic=WARN`, even `<cats>;<handlers>`). No shell-quoting worries — it's an env value. |
| `FOLLY_LOGGING` | folly | folly's own var (`folly::Init` reads it). Works, but it's not moqx-namespaced. |

`MOQX_LOGGING` is a thin alias: on startup moqx copies it into `FOLLY_LOGGING`
(before `folly::Init`) **only if `FOLLY_LOGGING` isn't already set**. So the full
precedence, lowest to highest:

```
compiled-in default  <  MOQX_LOGGING  <  FOLLY_LOGGING  <  --logging flag
```

i.e. a `--logging=…` flag overrides any env var, and an explicit `FOLLY_LOGGING`
overrides `MOQX_LOGGING`. In the Docker image, `MOQX_LOGGING` is the documented knob
(`docker run -e MOQX_LOGGING=DBG2 …`, or `MOQX_LOGGING=` in `docker/.env`).

## The category hierarchy

Each XLOG call derives its category from the source file path at compile time, with build-system prefix stripping. `--logging=<category>=<level>` then targets a category and its children.

**Setting the root works today** (`--logging=DBG2`, `--logging=WARN`, etc.) — that's how you crank or quiet the entire stack.

**Per-category targeting is currently inconsistent across the stack.** Each layer (moqx, moxygen, fizz, wangle, mvfst) has its own `FOLLY_XLOG_STRIP_PREFIXES` set when it was compiled, and those prefixes don't always reduce the source paths to short names. Empirically:

- moqx's own sources land under the `moqx.*` prefix in build configurations where `-fmacro-prefix-map=src=moqx` is applied (see `CMakeLists.txt`).
- moxygen sources currently land under the full absolute build path (e.g. `home.…moqx.deps.moxygen.moxygen.MoQServer`) because moxygen's strip-prefix references moqx's source root rather than moxygen's.
- The QUIC stacks (mvfst, picoquic) similarly carry compile-time-specific prefixes.

Until the cross-project category derivation is normalized (tracked as a follow-up to [#339](https://github.com/openmoq/moqx/issues/339)), the reliable operational pattern is:

```bash
--logging=DBG2          # everything at DBG2 (works regardless of category prefixes)
--logging=WARN          # mute everything to WARN
--logging=INFO,quic=WARN   # bare-level root + a coarse subtree mute
```

Coarse subtree mutes like `quic=WARN` work whenever the actual category starts with that prefix — they're the safest per-layer knob today.

**Discovering the actual category for a log line:** moqx's default handler uses folly's `GlogStyleFormatter`, which shows `file:line` but not the category name. A follow-up issue tracks adding a category-aware formatter option so operators can see categories at runtime.

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
| Log to file (stderr redirect) | `moqx … 2>> /var/log/moqx.log` (folly's `file:` handler isn't auto-registered) |
| Disable async (durable, slower) | `moqx … --log-handler=default:async=false` |
| Via env var (systemd / Docker / k8s) | `FOLLY_LOGGING=.=INFO,moqx=DBG2 moqx --config c.yaml` |

CLI flags win over the env var if both are set.

## File output

folly ships a `file:` handler type but **does not register it by default** — a config string with `file:path=…` could otherwise write to arbitrary paths. moqx doesn't register it either. To write logs to a file today, redirect at the launcher layer:

```bash
# shell
moqx --config c.yaml 2>> /var/log/moqx.log

# systemd unit
StandardError=append:/var/log/moqx.log

# docker (let the runtime handle rotation)
docker run --log-driver json-file --log-opt max-size=10m --log-opt max-file=5 \
  ghcr.io/openmoq/moqx:latest
```

If a project genuinely needs in-process file output via the folly config string, register `FileHandlerFactory` in `main.cpp` before `folly::Init`:

```cpp
folly::LoggerDB::get().registerHandlerFactory(
    std::make_unique<folly::FileHandlerFactory>());
```

That re-enables `--log-handler=default=file:path=…` at the cost of accepting whatever path the config string says. Don't do this in builds that run with elevated privileges.

## qlog (separately, for structured QUIC analysis)

XLOG and qlog have different jobs:

- **XLOG** is the ad-hoc operator/dev surface — code-embedded clues, controlled by `--logging`. Good for "what just went wrong."
- **qlog** is the IETF-standard structured QUIC log — JSON consumed by external tooling like [qvis](https://qvis.quictools.info/) for packet timelines, congestion control, stream events. Good for "trace a single session" investigations.

Per-session qlog enablement and random sampling are planned (the moqx admin surface will let you opt one connection in at a time). For now, enable qlog at the picoquic level for all connections:

```bash
moqx --config c.yaml --qlog-dir /tmp/qlogs --logging=quic.picoquic=INFO
```

Independent channels — enabling one doesn't suppress the other.

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
