# MOQT Relay Hops Implementation Plan

## Summary

Implement `draft-lcurley-moq-relay-hops` across moxygen and moqx, with the
draft as the normative source and the Red5 implementation used only as
supporting precedent.

The first release implements negotiation, wire formats, hop synthesis, loop
suppression, and `EXCLUDE_HOP` while preserving moqx's single-upstream model.
Optional multi-path selection and failover remain follow-up work.

## Implementation Changes

### 1. Add relay-hops protocol support to moxygen

- Define:
  - `RELAY_HOPS = 0x40B55`
  - `HOP_PATH = 0x40B57`
  - `EXCLUDE_HOP = 0x40B58`
- Extend parameter validation and draft-18 value encoding so:
  - `HOP_PATH` is length-prefixed and allowed only in `PUBLISH_NAMESPACE` and
    negotiated `NAMESPACE`.
  - `EXCLUDE_HOP` is a bare varint allowed only in `SUBSCRIBE_NAMESPACE`.
- Add version-aware hop-path helpers using QUIC varints for draft 16 and MoQ
  varints for drafts 17-18. Reject empty paths and incomplete or trailing
  varints as `PROTOCOL_VIOLATION`.
- Preserve the zero-length `RELAY_HOPS` setup option instead of removing it
  through the current setup-option allowlist.
- Extend `moxygen::Namespace` with message parameters.
- Add a negotiated relay-hops flag to `MoQSession` and propagate it into each
  request/response stream codec and frame writer.
- Encode and parse the appended `NAMESPACE` parameter block only when the
  session negotiated relay hops; leave non-negotiated framing byte-for-byte
  unchanged.
- Change `Publisher::NamespacePublishHandle::namespaceMsg` to receive the
  complete `Namespace`, updating existing implementations and samples so
  parameters survive the wire-to-application boundary.
- Give `MoQClientBase` a caller-controlled way to add setup options; only moqx
  relay peering enables `RELAY_HOPS`.

### 2. Negotiate the extension per session

- Outbound `UpstreamProvider` sessions offer the zero-length option.
- All moqx server backends advertise support, using the same setup-parameter
  injection API to keep mvfst, picoquic, and QMUX behavior identical.
- Mark negotiation successful:
  - Server-side only after receiving the client's advertisement.
  - Client-side only after receiving the server's advertisement.
- Do not infer negotiation from another session or from peer identity.
- Support the existing draft-16 peering path and drafts 17-18 framing already
  implemented by moxygen.

### 3. Implement relay policy in moqx

- Generate one random, nonzero Hop ID in `[1, 2^62)` per `MoqxRelayContext`,
  shared across its services and sessions for the process lifetime.
- Keep deterministic IDs injectable through constructors and test fixtures;
  add no production configuration toggle in this phase.
- Store the normalized incoming hop path with each `NamespaceTree` publisher
  entry.
- Use one canonical ingest path for both `PUBLISH_NAMESPACE` and `NAMESPACE`:
  - Negotiated session with a valid `HOP_PATH`: retain it.
  - Negotiated session without `HOP_PATH`: reject or drop the nonconforming
    advertisement without closing the session.
  - Non-negotiated source: synthesize a stable random origin Hop ID per source
    session.
  - Path containing this relay's Hop ID: drop without registering or
    forwarding.
  - Empty or malformed path: close with `PROTOCOL_VIOLATION`.
- Clear stored path state when the publisher is replaced, withdraws the
  namespace, or its session is removed.
- On every negotiated downstream advertisement:
  - Copy the stored path and append the local Hop ID.
  - Attach it to both `PUBLISH_NAMESPACE` and `NAMESPACE`.
  - For a legacy publisher, emit `[synthesized-origin, local-relay]`,
    preserving the draft's origin-first requirement.
- Parse `EXCLUDE_HOP` only on negotiated namespace subscriptions. Suppress an
  advertisement when the excluded ID occurs anywhere in its outgoing path,
  including the local hop just appended.
- Add the local Hop ID as `EXCLUDE_HOP` to moqx's wildcard upstream namespace
  subscription after negotiation.
- Continue sending plain advertisements and ignoring the extension on
  non-negotiated sessions.

## Public Interfaces

- `moxygen::Namespace` gains `TrackRequestParameters params`.
- `Publisher::NamespacePublishHandle::namespaceMsg` changes from a namespace
  suffix to the complete `Namespace`.
- `MoQSession` exposes relay-hops negotiation state and a stable synthesized
  source Hop ID.
- `MoQClientBase` exposes setup-option injection for specialized clients.
- `NamespaceTree::NamespaceNode` exposes its stored incoming hop path to relay
  forwarding.
- No YAML schema or operator configuration is added.

## Test Plan

- moxygen framing tests across drafts 16, 17, and 18:
  - Zero-length setup-option round trip.
  - Negotiated extended `NAMESPACE` round trip.
  - Legacy `NAMESPACE` remains unchanged.
  - One-entry and multi-entry paths, maximum supported IDs, empty path,
    truncated varint, and trailing bytes.
  - `EXCLUDE_HOP` even-key encoding.
  - Parameter/message allowlist enforcement.
- Session tests:
  - Offered and echoed negotiates.
  - Not offered, not echoed, or echoed unexpectedly does not negotiate.
  - Feature state is isolated per session and applied to newly created stream
    codecs.
- moqx relay tests:
  - The same legacy source receives a stable synthesized origin; different
    sessions receive different origins.
  - Loop-containing paths never enter `NamespaceTree`.
  - Negotiated forwarding appends exactly one local hop.
  - Legacy publishers produce `[stand-in, relay]`.
  - `EXCLUDE_HOP` suppresses matches in the origin, intermediate, and local-hop
    positions.
  - Non-negotiated subscribers receive plain advertisements.
  - Malformed paths close the correct source session.
- Integration:
  - Preserve the existing two-relay media-flow test.
  - Add a three-relay cycle whose returning advertisement is dropped and whose
    registration and advertisement counts remain stable during a settle
    window.
  - Add a legacy bridge case proving graceful degradation and stable
    synthesis.
- Verification order:
  1. Run focused moxygen framing and session tests.
  2. Build dependencies from the modified local submodule.
  3. Run focused moqx relay tests.
  4. Run `./scripts/format.sh --check`, the full CTest suite, sanitizer
     coverage, and `git diff --check`.
  5. Run `./scripts/docker-build.sh` as the machine-preferred clean build gate.

## Documentation and Delivery

- Keep `docs/draft-lcurley-moq-relay-hops.txt` as the normative repository
  reference.
- Update `docs/config.md` in place:
  - Replace the statement that loops are unsupported with negotiated loop
    prevention behavior.
  - Document legacy-peer degradation.
  - Retain and clearly state the single-upstream limitation.
- Do not create additional Markdown files.
- Deliver as four reviewable commits: moxygen wire support,
  negotiation/session plumbing, moqx relay behavior/tests, then
  integration/docs and the submodule pin update.

## Assumptions and Deferred Work

- The draft text takes precedence over Red5 design choices, notably requiring
  a synthesized origin before the relay's own hop for legacy publishers.
- Shortest-path selection, simultaneous candidate storage, failover, origin
  deduplication, topology coalescing or stripping, and multiple upstreams are
  optional draft behaviors and intentionally deferred.
- The existing modified moxygen checkout and local draft are preserved and
  incorporated deliberately; unrelated changes are not rewritten.
