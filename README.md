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

./scripts/build.sh setup     # prebuilt moxygen deps (~1 min)
./scripts/build.sh           # configure + build
./scripts/build.sh test      # run tests
```

## Dependency Modes

| Mode | Command | Time |
|------|---------|------|
| **prebuilt** | `build.sh setup` | ~1 min — downloads CI-built artifacts (requires `gh` CLI) |
| **local** | `build.sh setup --local` | 15-30 min — builds everything from source |

Prebuilt is the default. Falls back to local if artifacts aren't available.
Use `--no-fallback` to fail instead. Use `--clean` to wipe `.scratch/` first.

## Build Options

```
build.sh setup [--prebuilt|--local] [--no-fallback] [--clean]
build.sh [--preset default|san] [--build-dir DIR]
build.sh test [--build-dir DIR] [-- CTEST_ARGS...]
```

| Preset | Build dir | Description |
|--------|-----------|-------------|
| `default` | `build/` | RelWithDebInfo |
| `san` | `build-san/` | Debug + ASAN/UBSAN |

## Docker

```bash
docker pull ghcr.io/openmoq/o-rly:latest
```

See `docker/docker-compose.yml` for deployment with TLS.

## Architecture

`ORelay` is a hard fork of moxygen's `MoQRelay`, evolved independently
(threading, cache, chained relay support) while using moxygen's transport
libraries (MoQForwarder, MoQCache, MoQSession, MoQServer).

`ORelayServer` extends `MoQServer` to wire up `ORelay` as the
publish/subscribe handler.

## Design

- `design/ARCHITECTURE.md`
- `design/ROADMAP.md`
- `design/CI_OVERVIEW.md`
