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
- Updated existing configuration/build documentation and examples to explain
  the subset, provisional labels, key handling, and unsupported features.
- Pinned catapult's latest published release, v1.0.0, at
  `cc8aeb879c261c5b972b80e5bc426fceb8568e6d` for crypto primitives. The
  release's older token codec is not used for draft-02 wire parsing.

## Validation performed

- Default build passed; the resolved catapult source matched the v1.0.0 tag.
- Focused default CTest selection (C4m, Dpop, Auth, config resolver/serializer,
  issuer, filters) passed after the `aud`/`iss`, `catreplay`, DPoP, and
  `moqt-reval` changes (including publish): 283/283.
  The earlier 125/125 selection predates those changes.
- Sanitizer (ASAN/UBSAN/LeakSanitizer, `build/san`) run on the final code,
  including the `aud`/`iss`, `catreplay`, DPoP, and `unique_ptr` changes:
  330/330 passed across codec, DPoP, authentication, issuer, configuration, and
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
  algorithm/claim-label interoperability (PS256, ES384/EdDSA proofs, final
  `moqt`/`moqt-reval` labels).
- Replay state (`catreplay` bindings and DPoP proof `cti` values) is in memory
  per service: lost on restart and not shared between relays.
- Full default CTest on the final code (1548 tests): 11 failures among the 1518
  run with `UpstreamProviderTest` excluded (21 more skipped by the tests
  themselves), all in loopback/upstream tests: `MoqxRelayServerTest.H3TxnTimeout...`,
  `RelaySessionCloseTest` (2), `UpstreamClusterTest` (2),
  `UpstreamSetupCancelledTest`, `RelayUpstreamSubscribeRaceTest` (3, one
  segfault), `RelayUpstreamHungPeerTest`, `UpstreamCACertSignedServerTest`.
  `UpstreamProviderTest` (30 tests) mostly fails or hangs with a loopback
  connect `FutureTimeout`: of the 22 that finished, 4 passed; eight hung for
  about an hour and were killed, and eight never ran.
- Baseline: the pre-PR commit `a191e99b` was built in a separate worktree and
  all 11 failing tests above fail the same way there (failures, timeouts, and
  the same segfault), as do the `UpstreamProviderTest` cases checked (3). The
  failures predate this PR. The cause was not diagnosed, and the whole
  pre-PR suite was not run, so a regression in another test would not show as
  a difference.
- The live run covered one draft (18), HMAC only, and a small test MP4. The
  subscriber was authorized by `anonymous_claim` because `moqtextclient` can
  send only a type-0 token on SUBSCRIBE, so a token-carrying subscriber was not
  tested. ES256 and mid-stream expiry were not exercised live.
- No issue comment/closure was performed.

Detailed run output was recorded at `/tmp/moqx-971-validation.txt` and
`/tmp/moqx-971-full-tests.log` in the working environment.
