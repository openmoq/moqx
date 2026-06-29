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

For the underlying moxygen library architecture (session model, data plane,
threading, transport abstraction), see
[deps/moxygen/ARCHITECTURE.md](deps/moxygen/ARCHITECTURE.md).

`MoqxRelay` is a hard fork of moxygen's `MoQRelay`. We copy the relay core into
moqx so we can evolve it independently (threading model, custom cache miss
handling, chained caches, etc.) while still using moxygen's lower-level
building blocks as libraries:

- **MoQForwarder** — fan-out engine, used as-is from moxygen for now. May need
  to fork in the future to accommodate threading model differences.
- **MoqxCache** — object cache, hard-forked from moxygen. Customizable for moqx-specific functionality.
  and chained cache support may be upstreamed to openmoq/moxygen or maintained
  in our fork.
- **MoQSession / MoQServer / MoQRelaySession** — session and server
  infrastructure, used as libraries.

`MoqxRelayServer` extends `MoQServer` to wire up `MoqxRelay` as the publish/subscribe
handler and create `MoQRelaySession` instances for incoming connections.

## Documentation

- [docs/metrics.md](docs/metrics.md) — Prometheus metrics reference

## Design Documents

- [design/ci-architecture.md](design/ci-architecture.md) — CI pipelines, upstream sync, auto-deploy
- [design/configuration.md](design/configuration.md) — relay config file reference
- [design/gummy-bear.md](design/gummy-bear.md) — cache and forwarding design
- [design/hot-reloading.md](design/hot-reloading.md) — hot config reload
- [design/miss-handler.md](design/miss-handler.md) — cache miss handling

## Quick Start

> **Prerequisite: CMake 3.22+ is required.** All current targets ship a
> new-enough version out of the box: Ubuntu 22.04+, Debian 12+, recent
> macOS Homebrew. Verify with `cmake --version`. `build.sh` aborts early
> if cmake is missing or too old (override with `MOQX_SKIP_CMAKE_CHECK=1`
> if you know what you're doing).

```bash
git clone https://github.com/openmoq/moqx.git && cd moqx
git submodule update --init --recursive
sudo deps/moxygen/standalone/install-system-deps.sh   # system libs (both modes)

./scripts/build.sh setup     # download prebuilt deps (~1 min)
./scripts/build.sh           # build
./scripts/build.sh test      # test
```

System libraries are needed in **both** dependency modes — the moxygen
tarball ships folly/fizz/mvfst/proxygen statically, but its CMake config
still does `find_dependency(fmt, Glog, ...)` and folly itself transitively
needs OpenSSL/Boost. `build.sh setup`'s system-dep check only fires when
falling back to source, but the build step needs the libs regardless.

See [BUILD.md](BUILD.md) for full build and test instructions (dependency
modes, sanitizer profiles, Docker), and [RUNNING.md](RUNNING.md) for relay
operations.

## License

Apache 2.0 — see [LICENSE](LICENSE).
