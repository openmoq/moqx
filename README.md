# o-rly
The OpenMOQ Relay

## Prerequisites

- CMake 3.25+, Ninja, C++20 compiler (GCC 11+ / Clang 14+)
- System libraries: `build.sh setup` checks and reports what's missing

## Quick Start

```bash
git clone https://github.com/openmoq/o-rly.git && cd o-rly
git submodule update --init
sudo deps/moxygen/standalone/install-system-deps.sh   # Ubuntu/Debian

./scripts/build.sh setup     # download moxygen release artifacts (~1 min)
./scripts/build.sh           # configure + build
./scripts/build.sh test      # run tests
```

## Dependency Modes

| Mode | Command | Time |
|------|---------|------|
| **from-release** | `build.sh setup` | ~1 min — downloads CI-built artifacts (requires `gh` CLI) |
| **from-source** | `build.sh setup --from-source` | 15-30 min — builds everything from source |

Both accept an optional commit SHA to override the submodule pointer:

```bash
build.sh setup --from-release abc1234   # download artifacts for specific commit
build.sh setup --from-source abc1234    # build specific commit from source
```

To build against a local moxygen tree (useful when iterating on moxygen,
proxygen, folly, etc.):

```bash
build.sh setup --from-source --moxygen-dir ~/src/moxygen
```

Default (no SHA or dir) uses the current submodule HEAD.
Falls back from release to source if artifacts aren't available.
Use `--no-fallback` to fail instead. Use `--clean` to wipe `.scratch/` first.

## Build Options

```
build.sh setup [--from-release [SHA]|--from-source [SHA]] [--moxygen-dir DIR] [--no-fallback] [--clean]
build.sh [--profile default|san] [--build-dir DIR]
build.sh test [--build-dir DIR] [-- CTEST_ARGS...]
```

| Profile | Build dir | Description |
|---------|-----------|-------------|
| `default` | `build/` | RelWithDebInfo |
| `san` | `build-san/` | Debug + ASAN/UBSAN |

## Docker

```bash
docker pull ghcr.io/openmoq/o-rly:latest
```

See `docker/docker-compose.yml` for deployment with TLS.

## Architecture

### Relay Core: ORelay

`ORelay` is a hard fork of moxygen's `MoQRelay`. We copy the relay core into
o-rly so we can evolve it independently (threading model, custom cache miss
handling, chained caches, etc.) while still using moxygen's lower-level
building blocks as libraries:

- **MoQForwarder** — fan-out engine, used as-is from moxygen for now. May need
  to fork in the future to accommodate threading model differences.
- **MoQCache** — object cache, used as-is from moxygen. Custom miss handling
  and chained cache support may be upstreamed to openmoq/moxygen or maintained
  in our fork.
- **MoQSession / MoQServer / MoQRelaySession** — session and server
  infrastructure, used as libraries.

### ORelayServer

`ORelayServer` extends `MoQServer` to wire up `ORelay` as the publish/subscribe
handler and create `MoQRelaySession` instances for incoming connections.

## Design

- `design/ARCHITECTURE.md`
- `design/ROADMAP.md`
- `design/CI_OVERVIEW.md`
