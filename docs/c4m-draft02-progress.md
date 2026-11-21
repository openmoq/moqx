# C4M draft-02 implementation progress

Status on 2026-11-20, branch `feature/c4m-draft02` (PR 1015). All work is
committed and pushed. The implementation replaces the former CAT4MOQ token
format with a C4M draft-02 token core: HMAC/ES256 COSE tokens, `moqt` scopes,
standard claim validation (`exp`, `nbf`, `aud`, `iss`), `catreplay` 1, and DPoP,
with the limits listed under Remaining work.

## Completed

- Downloaded and checked in locally the draft text at
  [`ref/draft-ietf-moq-c4m-02.txt`](ref/draft-ietf-moq-c4m-02.txt) for review
  and vector reference.
- Added a bounded CBOR/COSE codec for tagged COSE_Mac0/HMAC-SHA256 and tagged
  COSE_Sign1/ES256, using the correct MAC0 and Signature1 structures.
- Implemented draft-02 action IDs and `moqt` scopes, including exact, prefix,
  and suffix field matching, namespace termination with `nil`, and fail-closed
  handling of unsupported restrictions.
- Enforced expiration and not-before claims, rejected duplicate claims and
  malformed/trailing encodings, and kept unsupported authorization claims
  from silently broadening permissions.
- Added ES256 public-key configuration for relay verification and a private-key
  issuer option. Keys are P-256; relay configuration loads public keys only.
  This addresses [issue #971](https://github.com/openmoq/moqx/issues/971).
- Added `aud`/`iss` validation: per-service `audiences`/`issuers` config,
  `ClaimPolicy` in the verifier, and `--auth_audience`/`--auth_issuer` issuer
  flags. Verified live: matching token publishes, wrong or missing audience
  is rejected.
- Added `catreplay` 1 enforcement: `cti`/`exp` required, token bound to the first
  accepting connection (per-service in-memory state), other connections refused
  (`AuthError::Replayed`). Issuer flag `--auth_replay_protect`. Verified live:
  second connection reusing the token was rejected.
- Added DPoP (draft section 4): `Dpop.{h,cpp}` proof verification and signing,
  `cnf`/`catdpop` parsing, per-action enforcement in `authenticateSetup` and
  `authorize`, proof `cti` replay cache, `auth.dpop_token_type`, and issuer flags
  `--auth_dpop_key_file`/`--auth_dpop_jkt`. Unit-tested against the draft's
  published `jkt` thumbprint vector.
- Added `moqt-reval` revalidation: `min_revalidation_interval_seconds`,
  `authorize()` now reports which grants keep an action alive, and the auth
  filters run a per-action timer that ends only the affected action on expiry
  (subscribe, fetch, subscribe_namespace, subscribe_tracks, publish_namespace,
  publish). The session itself is not revalidated.
- Added ES384 and EdDSA (Ed25519) DPoP proofs, the draft's SHOULD algorithms:
  generalized proof keys, `cnf` thumbprints for P-384 and OKP keys, and issuer
  binding to P-384/Ed25519 client keys. Thumbprints match RFC 9679 and RFC 8037
  vectors; no draft vectors exist for these algorithms.
- Updated existing configuration/build documentation and examples to explain
  the subset, provisional labels, key handling, and unsupported features.
- Pinned catapult's latest published release, v1.0.0, at
  `cc8aeb879c261c5b972b80e5bc426fceb8568e6d` for crypto primitives. The
  release's older token codec is not used for draft-02 wire parsing.

## Validation performed

- Default build passed; the resolved catapult source matched the v1.0.0 tag.
- Focused default CTest selection (C4m, Dpop, Auth, config resolver/serializer,
  issuer, filters) passed after the `aud`/`iss`, `catreplay`, DPoP, and
  `moqt-reval` changes (including publish) and the error-code mapping, and ES384/EdDSA proofs, and the live-run fixes: 293/293.
  The earlier 125/125 selection predates those changes.
- Sanitizer (ASAN/UBSAN/LeakSanitizer, `build/san`) run on the final code,
  including the `aud`/`iss`, `catreplay`, DPoP, and `unique_ptr` changes:
  340/340 passed across codec, DPoP, authentication, issuer, configuration, and
  revalidation filter tests (including the timer-driven ones). moqx and catapult crypto were instrumented; the prebuilt moxygen
  dependency was not.
- The draft's ES256 vector verifies; a tampered signature fails.
- Eight issuer CLI scenarios passed, covering direct/config ES256 issuance,
  missing or mismatched private keys, key ID selection, encrypted-key
  rejection without prompting, secret-safe errors, and retained HMAC issuance.
- Concurrent verification passed with 8 workers performing 64 verifications
  each.
- `clang-format-19 --dry-run --Werror` and `git diff --check` passed.
- Independent C++ review found no concrete issue above its reporting
  threshold.
- Live authenticated media delivery with the sibling `moqxr` publisher passed
  (MoQT draft 18, WebTransport, HMAC-SHA256 token from `moqx-issuer`, token
  type 1 via `--auth-profile c4m-01 --auth-token-file`):
  - no token: session rejected with "missing authorization token";
  - token for namespace `live`: namespace published with 3 tracks;
  - `moqtextclient` subscribed to `vide_1` and received `moof`/`mdat` media;
  - token for namespace `other` publishing `live`: rejected with
    "authorization token does not permit action".

## Remaining work and limitations

- Protocol conformance gaps are tracked in
  [C4M draft-02 implementation gaps](c4m-draft02-gaps.md). The major gaps are
  `moqt-reval` for `setup`, DPoP interoperability (provisional `actx`/`ath` labels and
  proof token type), composite claims (blocked on unassigned keys), and
  claim-label interoperability (final `moqt`/`moqt-reval` labels). PS256 tokens
  are not implemented but the draft does not define them.
- Replay state (`catreplay` bindings and DPoP proof `cti` values) is in memory
  per service: lost on restart and not shared between relays.
- Full default CTest on the final code, including `moqt-reval` for all actions
  (1561 tests, 120 s per-test timeout, run to completion): 37 failed and 21 were
  skipped by the tests themselves. All 37 failures are loopback/upstream
  tests: 11 outside `UpstreamProviderTest` (`MoqxRelayServerTest.H3TxnTimeout...`,
  `RelaySessionCloseTest` x2, `UpstreamClusterTest` x2,
  `UpstreamSetupCancelledTest`, `RelayUpstreamSubscribeRaceTest` x3 with one
  segfault, `RelayUpstreamHungPeerTest`, `UpstreamCACertSignedServerTest`) and 26
  of the 30 `UpstreamProviderTest` cases (loopback connect `FutureTimeout`,
  failing fast or hanging until the timeout). Every test added by this work
  passed.
- Baseline: the pre-PR commit `a191e99b` was built in a separate worktree and
  all 11 non-`UpstreamProviderTest` failures above fail the same way there
  (failures, timeouts, and the same segfault), as do the `UpstreamProviderTest`
  cases checked (3). The failures predate this PR. The cause was not diagnosed,
  and the whole pre-PR suite was not run, so a regression in another test would
  not show as a difference. The failure count (37) matches the first full run of
  this branch.
- Live revalidation and DPoP checks against the moqxr publisher (draft 18, HMAC):
  - `publish_namespace` revalidation: with a 9 s token and `moqt-reval` 1, the
    relay ended the action at expiry and the namespace left `/state` about 0.2 s
    later. The track subscription already open kept delivering, because the
    relay withdraws the announcement but does not stop tracks it is forwarding,
    and moqxr ignores the cancel.
  - The first live runs exposed two bugs the mock-based tests missed: (1) the
    session callback's cancel discards the session's handle, so the relay's
    route was never removed when the cancel came first (now the route is
    dropped first, with an ordering test); (2) the timer ran outside the request
    context moxygen uses to identify the owning session, so the request context
    is now captured when the action starts and re-entered when it is ended (with
    a regression test).
  - DPoP: a `cnf`-bound token with no proof, and with moqxr's draft-01 JWT
    proof, were both refused; an unbound token published. A positive DPoP run
    was not possible: no available client sends the draft-02 CWT proof.
  - Not run live: `publish`, `subscribe`, `fetch`, `subscribe_namespace` and
    `subscribe_tracks` revalidation (moqxr sent no PUBLISH in the mode tried,
    and moqtextclient cannot carry our binary tokens on a request).
- The earlier live run covered one draft (18), HMAC only, and a small test MP4. The
  subscriber was authorized by `anonymous_claim` because `moqtextclient` can
  send only a type-0 token on SUBSCRIBE, so a token-carrying subscriber was not
  tested. ES256 and mid-stream expiry were not exercised live.
- No issue comment/closure was performed.

Detailed run output was recorded at `/tmp/moqx-971-validation.txt` and
`/tmp/moqx-971-full-tests.log` in the working environment.
