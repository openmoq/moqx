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

`ORelayServer` extends `MoQServer` to wire up `ORelay` as the publish/subscribe
handler and create `MoQRelaySession` instances for incoming connections.

## Design Documents

- [design/ci-architecture.md](design/ci-architecture.md) — CI pipelines, upstream sync, auto-deploy
- [design/configuration.md](design/configuration.md) — relay config file reference
- [design/gummy-bear.md](design/gummy-bear.md) — cache and forwarding design
- [design/hot-reloading.md](design/hot-reloading.md) — hot config reload
- [design/miss-handler.md](design/miss-handler.md) — cache miss handling
