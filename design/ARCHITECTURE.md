# Architecture (reference implementation)

## Goals

- Provide a clear, readable C++20 reference for an OpenMOQ relay.
- Keep the transport layer aligned with QUIC best practices.
- Emphasize correctness and interoperability over optimization.
- Preserve portability: Linux first, macOS second, Ubuntu 20.04 baseline.

## Non-goals (initially)

- Full production hardening (rate limiting, multi-tenant quotas, etc.).
- Deep persistence or long-term storage.
- Vendor-specific optimizations.

## High-level components

- **Transport**
  - QUIC endpoint creation, connection lifecycle, crypto config.
  - Stream and datagram adapters used by MOQT sessions.

- **Protocol (MOQT)**
  - Session state machine.
  - Message encoder/decoder (frames, control messages, errors).
  - Capability and version negotiation.

- **Relay Core**
  - Route table and subscription graph.
  - Track cache (in-memory, bounded).
  - Forwarding policy (fanout, priority, eviction).

- **Store & Cache**
  - In-memory ring buffers for track data.
  - Optional disk-backed snapshots (future phase).

- **Observability**
  - Structured logging.
  - Metrics counters (connections, subscriptions, drops, latency).

- **Admin/Control Plane (optional)**
  - Local CLI or HTTP admin port for introspection.
  - Configuration reload.

## Process model

- Single-process daemon, event-driven.
- One I/O thread per QUIC endpoint (default 1).
- Worker pool for parsing and fanout (future phase).

## Data flow (simplified)

1. Client connects via QUIC, negotiates MOQT version.
2. Session validates capabilities and authorization (stubbed initially).
3. Client announces a track or subscribes to one.
4. Relay core resolves routes and distributes data to subscribers.

## Compatibility guidelines

- Keep wire framing isolated in `protocol/` so draft changes are localized.
- Version gates should be explicit and easy to reason about.
- Avoid undefined behavior and non-standard extensions.

## Directory mapping (planned)

- `src/transport/` QUIC bindings.
- `src/protocol/` MOQT framing and session.
- `src/relay/` route graph, cache, policies.
- `src/store/` in-memory cache, optional disk.
- `src/obs/` logging/metrics.
