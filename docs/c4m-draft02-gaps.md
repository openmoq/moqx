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
| `moqt-reval` processing (draft §2.2) | Implemented for integer intervals at or above `min_revalidation_interval_seconds` (default 5; 0 rejects all such tokens). The relay re-checks the authorizing token at the interval and ends only the affected action once it has expired: subscribe (PUBLISH_DONE `UNAUTHORIZED`), fetch (reset `EXPIRED_AUTH_TOKEN`), subscribe_namespace, subscribe_tracks, publish_namespace (cancel), and publish (relay unsubscribes from the publisher; the timer starts when the session awaits the PUBLISH reply). Not revalidated: the session itself. Unit-tested with mock handles and a real timer. Live (moqxr publisher, draft 18): `publish_namespace` revalidation withdrew the namespace and ended the publisher's track within milliseconds of expiry (the subscriber's stream ended with it); `publish` and the other actions have not been run live (moqxr sent no PUBLISH in the mode tried, and moqtextclient cannot carry our binary tokens on a request). Ending a `publish_namespace` ends the tracks that session sends under the namespace (including tracks under sub-namespaces it announced separately). | Decide whether an expired `setup` token should close the session. The only time-varying check is expiry, since keys are static, so key rotation would need a hook if keys ever reload. The `moqt-reval` label is still the private-use placeholder. |
| `catreplay` processing (draft §3) | Value 1 is enforced: requires `cti` and `exp`, binds the token to the first connection that presents a validly signed copy, allows reuse there, and refuses other connections until expiry. State is in-memory per service, bounded, not persisted or shared across relays or restarts. Value 2 is rejected. | Decide whether replay state must survive restarts or be shared across relay instances, and whether value 2 should map to value 1. |
| DPoP (draft §4) | Implemented for ES256 (MUST), ES384 and EdDSA/Ed25519 (SHOULD) proofs: `cnf` `ckt`/`jkt`, `catdpop` window and replay, COSE_Sign1 proof checks (typ, kccs key vs cnf thumbprint, signature, `ath`, `iat` window, exact `actx` action/namespace/track), enforced at SETUP and on every action including session-granted ones. `actx`/`ath` labels (-65539/-65540) and the proof token type (17) are provisional because the draft assigns neither and ships no proof vectors. Ed448 and other curves are not supported. ES384 and EdDSA were added without draft vectors; the thumbprints are checked against RFC 9679 and RFC 8037 vectors and the proofs round-trip only against moqx's own signer. | Adopt final labels and proof carriage when assigned; confirm interoperability against another implementation. The moqxr publisher's DPoP is the draft-01 JWT form and does not interoperate. |
| Composite claims (draft §2.1.1 and Appendix A.6) | Unsupported and rejected. Blocked: the `or`/`and`/`nor` claim keys are unassigned, the appendix vectors encode all three as key `0` (not a valid claim key) and distinguish them only by test metadata, and the semantics come from the external draft-lemmons-cose-composite-claims, which is not in the repo. | Implement once keys are allocated and the composite draft is available to review. |
| Algorithm coverage | Tokens: HMAC-SHA256 and ES256, the only two the draft defines (appendix: COSE_Mac0/HMAC-SHA256 and COSE_Sign1/ES256). DPoP proofs: ES256, ES384, EdDSA (Ed25519). PS256 tokens are not implemented; the draft does not define them, so this is not a conformance gap. | Add PS256 (or other CAT algorithms) only if a deployment needs them; catapult already provides `Ps256Algorithm` for the verifier side, and issuer support would be needed too. |
| Claim-label allocation | `moqt` and `moqt-reval` use Appendix private-use placeholders `-65537` and `-65538`; they are not assigned registry values. | Update the constants and interoperability vectors when the draft assigns final labels. Tokens using different labels will not interoperate with this build. |
| CWT encoding coverage | Decoder accepts definite-length CBOR and enforces local token/scope/namespace limits. Floating-point NumericDates and unsupported forms are rejected. | Check these restrictions against deployment interoperability needs and broaden only where the relevant CWT/COSE requirements permit it. |

## Error codes

- A request carrying an expired token fails with request error `EXPIRED_AUTH_TOKEN`
  (0x5); other refusals stay `UNAUTHORIZED`. Request error codes are sent unchanged
  in every draft by moxygen, so drafts 16, 18 and 22 behave the same; draft 22's
  text was not available to check. A token that cannot be decoded fails with
  `MALFORMED_AUTH_TOKEN` (0x4). A bad signature, an unsupported claim, or no token
  stays `UNAUTHORIZED`: the draft's "otherwise invalid" wording could be read to
  cover a bad signature too, but its code description is about serialization.
- Revalidation endings use PUBLISH_DONE `UNAUTHORIZED` (the draft's code for a
  subscriber that is no longer authorized; `EXPIRED` means the SUBSCRIBE_OK timeout)
  and, for fetch, stream reset `EXPIRED_AUTH_TOKEN` (0x7 from draft 18, `0` before).

## Integration and evidence gaps

- DPoP has unit coverage (including the draft's published `jkt` vector and RFC 9679 / RFC 8037 thumbprint vectors). Live, only the refusals were exercised against the moqxr publisher: a `cnf`-bound token with no proof, and with moqxr's draft-01 JWT proof, are both refused, while an unbound token publishes. A positive run needs a client that sends the draft-02 CWT proof (moqxr sends the draft-01 JWT form); none is available.
- Live revalidation run (moqxr publisher, draft 18, HMAC token with `moqt-reval` 1 and a 9 s lifetime): at expiry the relay logged the ending, the publisher's track ended for the subscriber within a millisecond, and `/state` showed the namespace gone by the next poll. The live run found two bugs that the mock tests could not (see the progress doc), both fixed.
- A live run with the sibling `moqxr` publisher passed (draft 18,
  WebTransport, HMAC token type 1): unauthenticated and wrong-namespace
  publishers were rejected, a correctly scoped publisher delivered media to a
  `moqtextclient` subscriber. Remaining gaps: the subscriber used
  `anonymous_claim` (the client cannot send a setup token), and ES256, other
  drafts, and mid-stream expiry were not exercised live.
- Full default CTest on the final code (1561 tests, run to completion): 37
  failures, all loopback/upstream tests (11 outside `UpstreamProviderTest`, 26
  of its 30 cases), plus 21 self-skipped. The pre-PR commit `a191e99b` fails the
  11 non-`UpstreamProviderTest` tests the same way (and the three
  `UpstreamProviderTest` cases checked), so they predate this PR; the cause is
  undiagnosed and the whole pre-PR suite was not compared. See the progress doc
  for the list.
- A prebuilt, uninstrumented moxygen dependency was used for sanitizer runs;
  moqx and catapult crypto were instrumented. The sanitizer results therefore
  do not cover moxygen internals.
- The implementation is not full draft-02 conformance while the protocol
  items above remain unsupported. Unsupported authorization restrictions are
  rejected rather than silently ignored.

## Validation snapshot

- Default build passed.
- Focused default tests (relay fixture in all three threading modes, C4m, Dpop,
  Auth, filters, config, issuer, registry, namespace tree, cross-executor
  filters): 731/731 passed on the final code.
- ASAN/UBSAN/LeakSanitizer on the auth, codec, config, registry and filter
  groups: 460 of 461 passed (the failure is the known loopback test
  `UpstreamCACertSignedServerTest`); the 336 relay fixture tests passed under
  them before the last review fixes, which do not touch code they exercise.
- Full default CTest: 37 failures of 1561, all pre-existing loopback/upstream
  tests (see the integration gap above); every test added by this work passed.
- Live moqxr publisher run: passed for HMAC, draft 18 (see above). Live checks
  also passed for `aud`/`iss` rejection and `catreplay` second-connection
  rejection.
