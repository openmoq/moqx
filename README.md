# o-rly
The OpenMOQ Relay

## Build

- `docs/BUILD.md`

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
