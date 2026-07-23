<div align="center">
  <img src="docs/banner.png" alt="moqx — Media Over QUIC Relay" />
</div>

<div align="center">

[![ci main](https://github.com/openmoq/moqx/actions/workflows/ci-main.yml/badge.svg)](https://github.com/openmoq/moqx/actions/workflows/ci-main.yml)
[![Latest release](https://img.shields.io/github/v/release/openmoq/moqx?display_name=tag&sort=semver&logo=github)](https://github.com/openmoq/moqx/releases/latest)
[![License](https://img.shields.io/github/license/openmoq/moqx)](LICENSE)
[![Last commit](https://img.shields.io/github/last-commit/openmoq/moqx)](https://github.com/openmoq/moqx/commits/main)
[![Open issues](https://img.shields.io/github/issues/openmoq/moqx)](https://github.com/openmoq/moqx/issues)
[![Open PRs](https://img.shields.io/github/issues-pr/openmoq/moqx)](https://github.com/openmoq/moqx/pulls)
[![MOQT](https://img.shields.io/badge/MOQT-draft--18-blue)](https://datatracker.ietf.org/doc/draft-ietf-moq-transport/)

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
scripts/install-system-deps.sh                         # toolchain + system libs
scripts/configure.sh --moxygen prebuilt-with-fallback  # get a moxygen, configure
scripts/build.sh                                       # == cmake --build build/default
scripts/test.sh                                        # == ctest --test-dir build/default --output-on-failure
```

`prebuilt-with-fallback` downloads the published moxygen for the pinned
`MOXYGEN_REV` when there is one, and compiles it when there isn't. moxygen decides
what it publishes, so that is the only setting guaranteed to produce a build.

Profiles (`default` | `san` | `tsan`) are each script's first argument and map
to `build/<profile>`; `--moxygen` picks where moxygen comes from:

| Goal | Command |
|------|---------|
| Just build it | `scripts/configure.sh --moxygen prebuilt-with-fallback && scripts/build.sh` |
| Download only, never compile moxygen | `scripts/configure.sh --moxygen prebuilt && scripts/build.sh` |
| Compile moxygen / any rev or platform | `scripts/configure.sh --moxygen from-source && scripts/build.sh` |
| Local moxygen checkout | `scripts/configure.sh --moxygen from-source --moxygen-dir /path && scripts/build.sh` |

Take `prebuilt` when you would rather see an error than wait — it fails instead of
compiling the folly stack, which is the slow surprise the fallback can hand you.

- How it works, and the raw-cmake equivalents: [BUILD.md](BUILD.md)
- Pins: [cmake/dependencies.cmake](cmake/dependencies.cmake)
- `ccache` used automatically

## Docs

- Build [BUILD.md](BUILD.md) · Run [RUNNING.md](RUNNING.md) · Metrics [docs/metrics.md](docs/metrics.md)
- Design: See [`design/`](design/)

## License

Apache 2.0 — see [LICENSE](LICENSE).
