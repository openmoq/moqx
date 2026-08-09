# moqx build system — workflows and requirements

Requirements for the dependency/build model, written against the direction
agreed on #519: the moxygen pin is authoritative, prebuilt when published,
automatic source fallback otherwise. Supersedes the earlier R1–R19 draft.

## Workflows

| # | Workflow | Shape under the pinned model |
|---|---|---|
| W1 | Everyday moqx PR | builds against the pin main carries; prebuilt path |
| W2 | Coupled moxygen+moqx change | PR pins the unmerged moxygen sha → CI source-builds it; after moxygen merges and the sync advances main's pin, rebase drops the deviation |
| W3 | Local dev loop | `configure.sh --moxygen prebuilt-with-fallback`; local moxygen via `--moxygen-dir` |
| W4 | Fresh clone / new platform | same command; unsupported platform lands on the source ladder |
| W5 | Reproduce / bisect | any commit rebuilds the set CI used; pin is in the tree |
| W6 | Daily sync | the sync job is the chief mover of the pin |
| W7 | Sanitizer investigation | nightly instrumented stack; PR lane covers moqx TUs only |
| W8 | Release cut (incl. hotfix branch) | resolves the pin to a published release |

## Requirements

**B1 — Resolution never dead-ends.** For any pin state — tagged, untagged,
unmerged, backward — every consumer (dev configure, PR CI, main CI, docker,
release) reaches a usable moxygen without human intervention: the prebuilt when
one is published for the pin, the source build otherwise. Degradation is loud,
and a kill-switch can hold a lane on prebuilt-only when a bad pin would
otherwise compile folly everywhere.

**B2 — The pin is authoritative and reproducible.** Every build uses exactly
the pinned revision or fails saying why. Nothing substitutes a different
revision silently — no tip fallback, no cache shadowing. Any checkout rebuilds
the dependency set its CI run used.

**B3 — The pin moves only deliberately.** The sync job is the normal mover.
A human bump is an explicit, reviewed act (CODEOWNERS on the pin file, or an
equivalent guard). Trying a different revision must not require editing the
tracked file, and an override that is ignored must say so rather than being
swallowed.

**B4 — Coupled changes stay one-step.** A moqx PR pinned at an unmerged
moxygen sha builds and tests in CI unaided. The follow-up (rebase after the
sync) is routine, not coordination.

**B5 — Merges are validated against current context.** A PR must not merge on
a green produced against a materially older pin. With the pin in a tracked
file this is enforceable by ordinary means (strict up-to-date or a merge
queue); pick one and turn it on.

**B6 — The resolved stack is visible.** One command reports the revisions
actually in use — moxygen, and the transitive stack it pins (picoquic, folly
and friends) — without digging through caches.

**B7 — CI economics hold.** Warm-cache PR wall-clock stays within budget; a
cold cache degrades speed, never outcome. The instrumented sanitizer stack
stays off the PR path, and losing a prebuilt must not silently convert a short
lane into an instrumented long one.

**B8 — Cross-repo interfaces are declared.** Where moqx depends on artifacts a
moxygen build flag produces (`BUILD_SAMPLES` binaries, `BUILD_TESTS` GTest
config), the dependency is checked at configure time naming the flag. Required
status-check names are stable identifiers; a rename lands with its ruleset
update or doesn't happen.

## Status at #519 head (551f2b87)

| Req | Status | Evidence |
|---|---|---|
| B1 | **verified** | `prebuilt-with-fallback` ladder exercised in a trial worktree: stale pin → 3s explanatory error → loud fallback → superbuild at the pinned rev. `MOQX_MOXYGEN_FALLBACK=off` kill-switch present. Docker uses the same ladder. Release lane still strict — see gaps. |
| B2 | **verified** | Stale pin cannot resolve to anything else; `-DMOXYGEN_REV=` override is ignored (file wins). Ignored silently — see B3. |
| B3 | open | No guard on the pin file, no sanctioned experiment path, swallowed override warns nothing. |
| B4 | **verified** | Same ladder: unmerged sha → no tag → source build at that sha, automatic in CI. |
| B5 | open | Repo setting, not the PR: `strict_up_to_date` off, no merge queue. |
| B6 | open | `print-pin.cmake` reports top-level pins only. |
| B7 | **verified** | Warm lanes 110–160s vs 360–660s on main; 180-min ceiling on the fallback; sanitizer interlock keeps fallback uninstrumented by design. |
| B8 | open | Superbuild hardcodes the flags; prebuilt path has only a load probe. Required check `asan debug` renamed without a ruleset update. |
