# o-rly

The OpenMOQ Relay — a MoQT relay server built on [moxygen](https://github.com/openmoq/moxygen).

## Quick Start

```bash
git clone https://github.com/openmoq/o-rly.git && cd o-rly
git submodule update --init
sudo deps/moxygen/standalone/install-system-deps.sh

./scripts/build.sh setup     # download prebuilt deps (~1 min)
./scripts/build.sh           # build
./scripts/build.sh test      # test
```

See [BUILD.md](BUILD.md) for full instructions: dependency modes, build profiles,
Docker deployment, relay operations, and debugging.

## Architecture

`ORelay` is a hard fork of moxygen's `MoQRelay`, evolved independently
(threading, cache, multi-service routing) while using moxygen's transport
libraries (MoQForwarder, MoQCache, MoQSession, MoQServer).

`ORelayServer` extends `MoQServer` to wire up `ORelay` as the
publish/subscribe handler.

See [BUILD.md](BUILD.md) for building, testing, deploying, and relay operations.

## Design Documents

- [design/ci-architecture.md](design/ci-architecture.md) — CI pipelines, upstream sync, auto-deploy
- [design/configuration.md](design/configuration.md) — relay config file reference
