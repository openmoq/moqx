# C4M draft-02 implementation gaps

Status on 2026-11-20, branch `feature/c4m-draft02`. This describes the
implementation on PR 1015 (all work committed and pushed) against the checked-in
[`draft-ietf-moq-c4m-02.txt`](ref/draft-ietf-moq-c4m-02.txt). It is a gap list,
not a claim of draft conformance.

## Implemented coverage

The current implementation supports tagged COSE_Mac0 with HMAC-SHA256 and
tagged COSE_Sign1 with ES256, draft-02 action IDs, `moqt` scope matching,
expiration and not-before checks, `aud`/`iss` validation against per-service
configuration, `catreplay` 1 connection binding, DPoP (`cnf`, `catdpop`, and
per-action ES256 proofs), and the draft's Appendix private-use claim labels. ES256 public-key verification and issuer signing address
[moqx issue #971](https://github.com/openmoq/moqx/issues/971). The legacy
CAT4MOQ wire format is no longer accepted.

Catapult v1.0.0 supplies cryptographic primitives. Its older token codec is not
used for draft-02 parsing; ES256 DER signatures are adapted to COSE's fixed
64-byte signature representation.

## Protocol gaps

| Draft requirement or behavior | Current behavior | Work needed |
|---|---|---|
| Standard CWT claim validation (draft §8.2.1) | `exp`, `nbf` enforced. `aud` and `iss` are validated against the per-service `audiences`/`issuers` config; a token with `aud` is rejected when no audience is configured. `sub`, `iat`, and `cti` remain informational unless `strict_claims` is enabled. | Decide whether `sub` needs relay-side meaning. Other unimplemented claims stay rejected. |
| `moqt-reval` processing (draft §2.2) | Implemented for integer intervals at or above `min_revalidation_interval_seconds` (default 5; 0 rejects all such tokens). The relay re-checks the authorizing token at the interval and ends only the affected action once it has expired: subscribe (PUBLISH_DONE `EXPIRED`), fetch (reset `EXPIRED_AUTH_TOKEN`), subscribe_namespace, subscribe_tracks, publish_namespace (cancel). Not revalidated: `publish` (the publisher-initiated PUBLISH has no relay-side hook with an executor) and the session itself. Unit-tested with mock handles and a real timer; not run end to end against a live publisher. | Revalidate `publish`; decide whether an expired `setup` token should close the session. The only time-varying check is expiry, since keys are static, so key rotation would need a hook if keys ever reload. The `moqt-reval` label is still the private-use placeholder. |
| `catreplay` processing (draft §3) | Value 1 is enforced: requires `cti` and `exp`, binds the token to the first connection that presents a validly signed copy, allows reuse there, and refuses other connections until expiry. State is in-memory per service, bounded, not persisted or shared across relays or restarts. Value 2 is rejected. | Decide whether replay state must survive restarts or be shared across relay instances, and whether value 2 should map to value 1. |
| DPoP (draft §4) | Implemented for ES256 proofs: `cnf` `ckt`/`jkt`, `catdpop` window and replay, COSE_Sign1 proof checks (typ, kccs key vs cnf thumbprint, signature, `ath`, `iat` window, exact `actx` action/namespace/track), enforced at SETUP and on every action including session-granted ones. `actx`/`ath` labels (-65539/-65540) and the proof token type (17) are provisional because the draft assigns neither and ships no proof vectors. ES384/EdDSA are not supported. | Adopt final labels and proof carriage when assigned; add ES384/EdDSA (SHOULD); confirm interoperability against another implementation. The moqxr publisher's DPoP is the draft-01 JWT form and does not interoperate. |
| Composite claims (draft §2.1.1 and Appendix A.6) | Unsupported and rejected. Blocked: the `or`/`and`/`nor` claim keys are unassigned, the appendix vectors encode all three as key `0` (not a valid claim key) and distinguish them only by test metadata, and the semantics come from the external draft-lemmons-cose-composite-claims, which is not in the repo. | Implement once keys are allocated and the composite draft is available to review. |
| Algorithm coverage | HMAC-SHA256 and ES256 tokens, and ES256 DPoP proofs, are implemented. PS256 tokens and ES384/EdDSA proofs are not. | Add any additional algorithms required by the selected CAT/draft interoperability profile, with matching issuer and verifier support. |
| Claim-label allocation | `moqt` and `moqt-reval` use Appendix private-use placeholders `-65537` and `-65538`; they are not assigned registry values. | Update the constants and interoperability vectors when the draft assigns final labels. Tokens using different labels will not interoperate with this build. |
| CWT encoding coverage | Decoder accepts definite-length CBOR and enforces local token/scope/namespace limits. Floating-point NumericDates and unsupported forms are rejected. | Check these restrictions against deployment interoperability needs and broaden only where the relevant CWT/COSE requirements permit it. |

## Integration and evidence gaps

- DPoP has unit coverage only (including the draft's published `jkt` vector). No live run: the moqxr publisher sends draft-01 JWT proofs, not the draft-02 CWT form.
- A live run with the sibling `moqxr` publisher passed (draft 18,
  WebTransport, HMAC token type 1): unauthenticated and wrong-namespace
  publishers were rejected, a correctly scoped publisher delivered media to a
  `moqtextclient` subscriber. Remaining gaps: the subscriber used
  `anonymous_claim` (the client cannot send a setup token), and ES256, other
  drafts, and mid-stream expiry were not exercised live.
- Full default CTest on the final code: 11 failures among 1518 tests run with
  `UpstreamProviderTest` excluded, plus widespread failures and hangs in
  `UpstreamProviderTest` itself, all loopback/upstream connect problems
  (`FutureTimeout`). The pre-PR commit `a191e99b` fails the same tests the same
  way, so they predate this PR; the cause is undiagnosed and only the failing
  tests (not the whole suite) were compared against it. See the progress doc
  for the list.
- A prebuilt, uninstrumented moxygen dependency was used for sanitizer runs;
  moqx and catapult crypto were instrumented. The sanitizer results therefore
  do not cover moxygen internals.
- The implementation is not full draft-02 conformance while the protocol
  items above remain unsupported. Unsupported authorization restrictions are
  rejected rather than silently ignored.

## Validation snapshot

- Default build passed.
- Focused default tests (C4m, Dpop, Auth, filters, config, issuer): 281/281
  passed after the `aud`/`iss`, `catreplay`, DPoP, and `moqt-reval` changes.
- ASAN/UBSAN/LeakSanitizer focused tests on the final code: 328/328 passed
  across codec, DPoP, authentication, issuer, configuration, and revalidation
  filter groups.
- Full default CTest: see the integration gap above (11 pre-existing upstream
  failures outside `UpstreamProviderTest`).
- Live moqxr publisher run: passed for HMAC, draft 18 (see above). Live checks
  also passed for `aud`/`iss` rejection and `catreplay` second-connection
  rejection.
