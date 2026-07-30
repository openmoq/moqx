# moqx build system — workflows and derived requirements

Working doc for the #519 discussion. Workflows first, requirements derived from
them. Requirements are stated design-neutral — they don't presuppose either
the current approach or the PR's.

## Workflows

| # | Workflow | Actor | Frequency |
|---|---|---|---|
| W1 | moqx-only change, no moxygen involvement | any dev | ~daily |
| W2 | Coupled moqx + moxygen change, both PRs in flight | dev | weekly |
| W3 | Coupled picoquic + moxygen + moqx change | dev | rare |
| W4 | Local iteration against an edited moxygen checkout | dev | weekly |
| W5 | First build on a fresh clone | new contributor | onboarding |
| W6 | Reproduce a CI failure locally | dev | as needed |
| W7 | Bisect a regression across the moxygen boundary | dev | rare |
| W8 | Sanitizer / TSan investigation | dev | as needed |
| W9 | Automated moxygen sync | cron | daily 08:23 |
| W10 | Cut a moqx release | release owner | on demand |
| W11 | Hotfix on a `release/vX.Y` branch | release owner | as needed |
| W12 | Hold at last-known-good while moxygen tip is broken | anyone | incident |
| W13 | Build on a platform with no published prebuilt | dev / user | as needed |
| W14 | Fresh CI runner, cold caches | CI | every eviction |
| W15 | Test against a different published moxygen rev | dev | as needed |
| W16 | Change a moxygen build flag (`BUILD_TESTS`, `BUILD_SAMPLES`) | moxygen dev | rare |

### Where the two designs diverge

Only the workflows that discriminate are worth meeting time.

**W2 — coupled moqx + moxygen.**
Today: `ahead_by != 0` auto-selects source build; CI green with no extra action.
#519: `--mode from-source` exists but CI only invokes the prebuilt path.

**W16 — change a moxygen build flag.**
`BUILD_TESTS` and `BUILD_SAMPLES` have historically thrashed: turned off as an
obvious saving, turned back on after something distant broke. The coupling is
undeclared in both designs. `test_conformance.sh` needs
`$MOQBIN/moqtest_{client,server}` from moxygen's install, and moqx's tests need
the GTest config that moxygen's `BUILD_TESTS` produces. The superbuild passes
both flags explicitly; the prebuilt path gets whatever the publish job happened
to build, with only a defensive `if(EXISTS .../moqtest_client)` load probe as a
trace. Dropping samples to shrink the tarball would break moqx conformance at
test runtime, in a different repo, with nothing linking cause to effect.

**W15 — test against a different published rev.**
Today: `build.sh setup --from-release <sha>`. No tracked file changes; the
submodule pointer moves but `ignore = all` keeps it out of `status` and `add`.
#519: no override path exists. `MOXYGEN_REV` is a plain `set()`, not `CACHE`,
so `-DMOXYGEN_REV=` is overwritten every configure; `CPM_moxygen_SOURCE` takes
a local checkout and forces a from-source build; `MOXYGEN_RELEASE_TAG` still
asserts the release commit equals the pin. **Editing
`cmake/dependencies.cmake` is the only path** — which leaves a staged-by-default
change in the tree every time anyone tries another rev.

**W6 / W7 — reproduce and bisect.**
Today: not reproducible — CI builds against moxygen tip, which moves.
#519: deterministic from the commit alone.

**W12 — incident hold.**
Today: a backward pin is silently discarded (`ahead_by == 0` → tip).
#519: a backward pin hard-fails. Neither delivers the workflow.

**W14 — cold runner.**
Today: cascades tarball → dev-build artifact → source build.
#519: `FATAL_ERROR`. Warm caches currently mask this.

**W5 — first build.**
Today: submodule init + `build.sh setup` + `build.sh`.
#519: `cmake --preset default`.

**W8 — sanitizers.**
Today: ASan over uninstrumented deps only.
#519: adds a nightly fully-instrumented stack, plus TSan.

**W10 / W11 — release.**
Today: resolves a moxygen tag from input or `snapshot-latest`.
#519: resolves live from the pin; fails if no release points at it.

**W3 — three-repo coupled.** Unchanged by the PR. picoquic is pinned
transitively via moxygen's `build/deps/github_hashes/openmoq/picoquic-rev.txt`,
so the chain is moqx → moxygen → picoquic and each hop needs its own PR.

## Requirements

### Correctness and availability

- **R1 — No dead ends.** For any pin state, CI must reach a usable moxygen
  without human intervention. Slower is acceptable; failing is not. *(W2, W12, W14)*
- **R2 — Explicit degradation.** When the fast path is unavailable, the build
  degrades to a slower path and says so in the log. *(W14)*
- **R3 — Pre/post-merge parity.** A moqx PR must resolve moxygen the same way
  main will after merge. *(W1)*
- **R17 — Cross-repo build flags are declared, not discovered.** Where moqx
  depends on an artifact produced by a moxygen build flag, the dependency is
  checked at configure time and the message names the flag. A flag change in
  moxygen must not surface as a runtime failure in moqx. *(W16)*

### Integration signal

- **R4 — Attribution window.** A moxygen regression must be attributable to a
  single upstream commit. Batching N commits behind a daily sync makes
  attribution a bisect. *(W1, W9)*
- **R5 — Coupled-change support in CI.** A moqx PR pinning an unmerged moxygen
  sha must build and test in CI without editing workflow files. *(W2, W3)*

### Determinism

- **R6 — Reproducible from the commit.** A checkout of any moqx commit must
  rebuild the same dependency set that CI used for it. *(W6, W7)*
- **R7 — Authoritative explicit pins.** When a pin is set deliberately, the
  build uses it or fails loudly. It is never silently overridden. *(W12)*
- **R8 — Backward pinning.** It must be possible to hold at an older
  known-good moxygen while upstream is broken. *(W12)*
- **R9 — Transitive visibility.** The full resolved stack — moxygen, picoquic,
  folly and friends — must be readable from a moqx checkout without manual
  archaeology. *(W3, W6)*
- **R16 — Pin changes require explicit intent.** It must not be possible to
  sweep a dependency pin change into an unrelated commit. Experimenting with a
  different rev must not require editing a tracked file. *(W15)*

> R4 and R6 are in tension. Tip-tracking optimizes R4; exact-pin optimizes R6.
> Satisfying both means two modes, not one.

### Developer experience

- **R10 — Cold start.** Fresh clone to running binary with documented steps and
  no manual dependency staging. *(W5)*
- **R11 — Local moxygen loop.** Build moqx against an edited local moxygen
  checkout without publishing anything. *(W4)*
- **R12 — Unsupported platforms.** Any platform without a published prebuilt
  must have a documented from-source path. *(W13)*

### Cost

- **R13 — PR wall-clock.** Per-PR CI must stay within budget on a warm cache
  and must not fail on a cold one. *(W1, W14)*
- **R14 — Sanitizer coverage.** Instrumented builds of the full stack must be
  reachable, on a cadence that doesn't gate PRs. *(W8)*

### Release

- **R15 — Release independence.** Cutting a release must not depend on
  transient upstream state (a moving tag, a rate-limited API). *(W10, W11)*

## Scoring

| Req | Today | #519 | Notes |
|---|---|---|---|
| R1 no dead ends | yes | **no** | `FATAL_ERROR`, no cascade |
| R2 explicit degradation | yes | n/a | |
| R3 pre/post parity | yes | yes | both internally consistent |
| R4 attribution window | **yes** | no | tip vs daily batch |
| R5 coupled in CI | **yes** | no | no from-source lane |
| R6 reproducible | no | **yes** | |
| R7 authoritative pins | no | **yes** | backward pin silently dropped today |
| R8 backward pinning | no | no | neither delivers |
| R9 transitive visibility | partial | partial | source not in tree under CPM |
| R10 cold start | partial | **yes** | `cmake --preset default` |
| R11 local moxygen loop | yes | yes | `--moxygen-dir` / `CPM_moxygen_SOURCE` |
| R12 unsupported platforms | yes | yes | |
| R13 PR wall-clock | partial | **yes** warm / **no** cold | main has no caching at all |
| R14 sanitizer coverage | no | **yes** | new nightly instrumented lane |
| R15 release independence | partial | no | live tag resolution at validate |
| R16 pin-change intent | by hiding | **no** | see below — the two are coupled |
| R17 declared cross-repo flags | no | no | pre-existing; #519 adds call sites |

## R9 and R16 are the same tradeoff

Today satisfies R16 only because `ignore = all` makes the submodule pointer
invisible to `status` and `add`. That same invisibility is why R9 scores
partial: you can't see what you're pinned to either. Hiding the pin protects
it and obscures it in one stroke.

Neither design should keep that tradeoff. Two changes settle both:

- **No-edit override.** Read `MOXYGEN_REV` from the environment when set,
  falling back to the file, with a `message(WARNING)` naming the override.
  Preserves the plain-`set()` rationale (a cached pin must not shadow a bump)
  while removing any reason to edit a tracked file.
- **CI guard.** Fail a PR that modifies `cmake/dependencies.cmake` unless the
  branch is `sync/**` or the PR carries a `pin-bump` label.

The guard is a genuine improvement over `ignore = all`, not a replacement for
it: it surfaces a pin change and demands intent, where `ignore = all` merely
made one hard to commit by accident. An intentional bump gets reviewed as a
bump.

## Reading

Neither design satisfies the full set. #519 wins R6, R7, R10, R13-warm, R14.
Today wins R1, R2, R4, R5, and R16-by-accident. R8 is unmet by both.

The gaps in #519 (R1, R5, R16) are additive — a fallback cascade, a
from-source CI lane, an override plus a guard. The gaps in today's setup
(R6, R7, R14) are what the PR was built to close and would need a rebuild to
reach. That asymmetry is why the PR's foundation plus follow-ups is the
cheaper path.

## Status

Agreed with Michal: tip-tracking semantics and the cascade to source are
preserved. **Not yet implemented** — branch head `c7bb4f23` still has
`FetchMoxygenPrebuilt` raising `FATAL_ERROR` with no fallback and no tip mode.
Track as explicit follow-up commits on the PR.

Landed since the first review: `GITHUB_TOKEN` passthrough (rate-limit failure)
and memory-aware job capping in `scripts/lib/jobs.sh` (sanitizer OOM). The
asan lane's 300s → 796s regression is the RAM derating, not a fixed penalty —
the queued linode resize reclaims most of it.
