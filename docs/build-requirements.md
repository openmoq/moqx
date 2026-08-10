# moqx build system — workflows and requirements

Requirements for the dependency/build model, written against the direction
agreed on #519: the moxygen pin is authoritative, prebuilt when published,
automatic source fallback otherwise. Supersedes the earlier R1–R19 draft.

## Background

moqx is built on moxygen, which itself wraps the Meta C++ stack (folly, mvfst,
proxygen). Compiling that stack from scratch takes on the order of half an
hour, so moxygen's CI publishes ready-made builds ("prebuilts") as GitHub
release assets, one tarball per platform.

Until #519, moqx tracked moxygen with a git submodule, and CI usually built
against the newest moxygen rather than the submodule's exact commit. #519
replaces that: one pinned commit hash in `cmake/dependencies.cmake` names the
moxygen every build uses. If a prebuilt exists for that commit, the build
downloads it; if not, the build compiles moxygen from source automatically.

Terms used throughout:

- **the pin** — `MOXYGEN_REV` in `cmake/dependencies.cmake`: the moxygen
  commit this moqx builds against
- **prebuilt** — a ready-made moxygen build downloaded from a moxygen GitHub
  release
- **source build / superbuild** — the helper project that compiles moxygen and
  its dependency stack locally when no prebuilt fits; slow but works for any
  commit on any platform
- **fallback ladder** — try the prebuilt first, fall back to the source build,
  announcing the switch
- **snapshot vs `v*`** — moxygen publishes a rolling `snapshot-latest` release
  that moves with every merge, and permanent `v1.2.3` releases; only the
  latter are durable references
- **sync job** — the daily automation that advances moqx's pin to moxygen's
  newest commit via a reviewed PR

## Workflows

| # | Workflow | Shape under the pinned model |
|---|---|---|
| W1 | Everyday moqx PR | builds against the pin main carries; prebuilt path |
| W2 | Coupled moxygen+moqx change | PR pins the unmerged moxygen sha → CI source-builds it; after moxygen merges and the sync advances main's pin, rebase drops the deviation |
| W3 | Local dev loop | `configure.sh --moxygen prebuilt-with-fallback`; local moxygen via `--moxygen-dir` |
| W4 | Fresh clone / new platform | same command; unsupported platform lands on the source ladder |
| W5 | Reproduce / bisect | any commit rebuilds the set CI used; pin is in the tree |
| W6 | Daily sync | the sync job is the chief mover of the pin |
| W7 | Sanitizer investigation | a nightly job builds the whole stack with sanitizers compiled in; the per-PR check sanitizes moqx's own code only |
| W8 | Release cut (incl. hotfix branch) | resolves the pin to a published release |

## Requirements

**B1 — Resolution never dead-ends.** For any pin state — tagged, untagged,
unmerged, backward — every consumer (dev configure, PR CI, main CI, docker,
release) reaches a usable moxygen without human intervention: the prebuilt when
one is published for the pin, the source build otherwise. When the build takes
the slower path it says so plainly in the log and in the CI summary. There is
also an off switch (`MOQX_MOXYGEN_FALLBACK=off`): if a bad pin ever sends
every CI job into a half-hour source build, an operator can disable the
fallback so jobs fail fast until the pin is fixed.

**B2 — The pin is authoritative and reproducible.** Every build uses exactly
the pinned revision or fails saying why. Nothing substitutes a different
revision silently — no tip fallback, no cache shadowing. Any checkout rebuilds
the dependency set its CI run used.

**B3 — The pin moves only deliberately.** The sync job is the normal mover.
A human bump is an explicit, reviewed act (CODEOWNERS on the pin file, or an
equivalent guard). Trying a different revision must not require editing the
tracked file — a developer testing against another moxygen locally should have
an environment/command-line path that leaves no change to accidentally commit,
so their PR diff stays exactly their change. (Deliberately pinning a PR to an
unmerged moxygen is different: there the file edit is the point, and review
covers it.) And an override that is ignored must say so rather than being
silently swallowed.

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

**B7 — CI economics hold.** With caches warm, per-PR CI time stays within
budget; a cold cache makes jobs slower, never red. Sanitizer builds of the
full dependency stack run nightly, not per-PR — and losing a prebuilt must not
silently turn a minutes-long PR check into a multi-hour sanitized build.

**B8 — Cross-repo interfaces are declared.** Where moqx depends on artifacts a
moxygen build flag produces (`BUILD_SAMPLES` binaries, `BUILD_TESTS` GTest
config), the dependency is checked at configure time naming the flag. Required
status-check names are stable identifiers; a rename lands with its ruleset
update or doesn't happen.

## Status at #519 head (551f2b87)

| Req | Status | Evidence |
|---|---|---|
| B1 | **verified** | fallback exercised in a trial worktree: stale pin → 3s explanatory error → announced fallback → source build at the pinned rev. Fallback off switch present. Docker uses the same ladder. The release path stays deliberately strict — see gaps. |
| B2 | **verified** | Stale pin cannot resolve to anything else; `-DMOXYGEN_REV=` override is ignored (file wins). Ignored silently — see B3. |
| B3 | open | No guard on the pin file, no sanctioned experiment path, swallowed override warns nothing. |
| B4 | **verified** | Same ladder: unmerged sha → no tag → source build at that sha, automatic in CI. |
| B5 | open | Repo setting, not the PR: `strict_up_to_date` off, no merge queue. |
| B6 | open | `print-pin.cmake` reports top-level pins only. |
| B7 | **verified** | warm-cache CI jobs 110–160s vs 360–660s on main today; jobs carry a 180-minute ceiling to accommodate the fallback; a guard stops the fallback silently changing what a sanitizer job tests. |
| B8 | open | Superbuild hardcodes the flags; prebuilt path has only a load probe. Required check `asan debug` renamed without a ruleset update. |
