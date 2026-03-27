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

See [BUILD.md](BUILD.md) for full build, test, deploy, and operations instructions.

## Architecture

`ORelay` is a hard fork of moxygen's `MoQRelay`, evolved independently
(threading, cache, multi-service routing) while using moxygen's transport
libraries (MoQForwarder, MoQCache, MoQSession, MoQServer).

`ORelayServer` extends `MoQServer` to wire up `ORelay` as the
publish/subscribe handler.

## Design Documents

- [design/ci-architecture.md](design/ci-architecture.md) — CI pipelines, upstream sync, auto-deploy
- [design/configuration.md](design/configuration.md) — relay config file reference
- [design/gummy-bear.md](design/gummy-bear.md) — cache and forwarding design
- [design/hot-reloading.md](design/hot-reloading.md) — hot config reload
- [design/miss-handler.md](design/miss-handler.md) — cache miss handling
