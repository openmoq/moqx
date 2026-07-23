<div align="center">
  <img src="docs/banner.png" alt="moqx — Media Over QUIC Relay" />
</div>

<div align="center">

[![ci main](https://github.com/openmoq/moqx/actions/workflows/ci-main.yml/badge.svg)](https://github.com/openmoq/moqx/actions/workflows/ci-main.yml)
[![ci pr](https://github.com/openmoq/moqx/actions/workflows/ci-pr.yml/badge.svg)](https://github.com/openmoq/moqx/actions/workflows/ci-pr.yml)
[![Latest release](https://img.shields.io/github/v/release/openmoq/moqx?display_name=tag&sort=semver&logo=github)](https://github.com/openmoq/moqx/releases/latest)
[![License](https://img.shields.io/github/license/openmoq/moqx)](LICENSE)
[![Last commit](https://img.shields.io/github/last-commit/openmoq/moqx)](https://github.com/openmoq/moqx/commits/main)
[![Open issues](https://img.shields.io/github/issues/openmoq/moqx)](https://github.com/openmoq/moqx/issues)
[![Open PRs](https://img.shields.io/github/issues-pr/openmoq/moqx)](https://github.com/openmoq/moqx/pulls)
[![MOQT](https://img.shields.io/badge/MOQT-draft--16-blue)](https://datatracker.ietf.org/doc/draft-ietf-moq-transport/)

</div>

# moqx

The OpenMOQ Relay — a MoQT relay server based on
[moxygen](https://github.com/openmoq/moxygen)
(upstream: [facebookexperimental/moxygen](https://github.com/facebookexperimental/moxygen)).

## Architecture

`MoqxRelay` is a hard fork of moxygen's
[`MoQRelay`](https://github.com/openmoq/moxygen/blob/main/moxygen/relay/MoQRelay.h),
so the relay core can evolve independently while the lower-level moxygen pieces
stay libraries:

- **MoQForwarder** — fan-out engine
- **MoqxCache** — object cache
- **MoQSession / MoQServer / MoQRelaySession** — session/server infrastructure.

`MoqxRelayServer` extends `MoQServer` to wire `MoqxRelay` in as the
publish/subscribe handler. For moxygen's own architecture, see its
[ARCHITECTURE.md](https://github.com/openmoq/moxygen/blob/main/ARCHITECTURE.md).

## Quick Start

Standard CMake preset build (CMake 3.23+, C++20, Ninja).

```bash
scripts/install-system-deps.sh         # toolchain + system libs (both modes)
scripts/configure.sh --mode prebuilt   # or --mode from-source; the choice is explicit
scripts/build.sh                       # == cmake --build build/default
scripts/test.sh                        # == ctest --test-dir build/default --output-on-failure
```

Profiles (`default` | `san` | `tsan`) are each script's first argument and map
to `build/<profile>`; `configure.sh` picks where moxygen comes from — download
a prebuilt, or build from source:

| Goal | Command |
|------|---------|
| Prebuilt moxygen | `scripts/configure.sh --mode prebuilt && scripts/build.sh` |
| Build moxygen from source / any rev | `scripts/configure.sh --mode from-source && scripts/build.sh` |
| Local moxygen checkout | `scripts/configure.sh --mode from-source --moxygen-dir /path && scripts/build.sh` |

- How it works, and the raw-cmake equivalents: [BUILD.md](BUILD.md)
- Pins: [cmake/dependencies.cmake](cmake/dependencies.cmake)
- `ccache` used automatically

## Docs

- Build [BUILD.md](BUILD.md) · Run [RUNNING.md](RUNNING.md) · Metrics [docs/metrics.md](docs/metrics.md)
- Design: See [`design/`](design/)

## License

Apache 2.0 — see [LICENSE](LICENSE).
