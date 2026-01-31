# Roadmap (MOQT draft 14–16)

This roadmap outlines probable phases to evolve the relay alongside MOQT drafts 14 through 16. The intent is to keep wire changes localized and preserve interoperability testing at each step.

## Phase 0: Repo + build foundation (now)

- Establish CMake + scripts + Docker build layout.
- Minimal relay binary with test harness scaffold.
- Logging and metrics stubs.

## Phase 1: Draft 14 baseline

**Goals**
- Implement core session lifecycle and basic message parsing/serialization.
- Support minimal announce/subscribe flows needed for interop tests.
- Provide a working in-memory relay path: publisher → relay → subscriber.

**Deliverables**
- Protocol framing and decoder with strict error handling.
- Simple route table (track namespace → subscribers).
- Basic cache policy (last-N objects per track).
- End-to-end smoke test using local clients (to be added).

## Phase 2: Draft 15 deltas + interoperability

**Goals**
- Update wire format and negotiation to match draft 15 deltas.
- Improve compatibility via explicit version gating and feature flags.
- Expand test matrix to cover backward/forward mismatch cases.

**Deliverables**
- Version negotiation logic with strict draft 14/15 separation.
- Draft 15 framing updates localized to `protocol/`.
- Interop test vectors for upgrade/downgrade paths.

## Phase 3: Draft 16 stabilization

**Goals**
- Incorporate draft 16 changes while retaining a reference-friendly codebase.
- Stabilize routing, cache eviction, and error semantics.
- Add observability for spec validation and performance baselines.

**Deliverables**
- Draft 16 protocol updates with minimal churn.
- Metrics for connection/session/track lifecycle.
- Benchmarks for fanout and cache behavior.

## Cross-cutting work items

- **Conformance testing**: develop a small scripted test harness for CI.
- **Compatibility**: keep draft-specific logic isolated and well documented.
- **Security**: input validation, limits, and denial-of-service safeguards.
- **Documentation**: example config and operational guidance for operators.

## Draft transition policy (expected)

- Each draft is implemented behind explicit gates.
- Default behavior targets the latest supported draft.
- Backward compatibility should be tested, not assumed.
