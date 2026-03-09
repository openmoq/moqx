# Two-Branch Architecture for openmoq/moxygen

## Context

The current single-branch approach has compounding problems: patches modify
upstream files creating perpetual conflicts, getdeps builds 30+ deps producing
1.7 GB tarballs, and the sync workflow has grown complex. The standalone build
(upstream PR #99, merged) eliminates most of this: no manifests, 5 FetchContent
deps, ~50-150 MB artifacts.

## The Full Flow

### Branches

- **`main`** — Pure upstream mirror. Unmodified copy of latest green upstream
  commit. No openmoq files. No workflows run. Not the default branch.
- **`openmoq-main`** (DEFAULT) — Working branch. All openmoq customizations.
  Developer PRs go here. Artifacts published from here.

### Daily sync cycle

```
upstream (facebookexperimental/moxygen)
  │
  │  1. Sync workflow scans last 20 commits, picks newest green one
  │
  ▼
main (pure mirror)
  │  2. Fast-forward push: main = upstream green commit
  │     No workflows fire (all disabled on fork)
  │
  │  3. Check: is main already ancestor of openmoq-main?
  │     YES → done, nothing to merge
  │     NO  → continue
  │
  ▼
PR: main → openmoq-main
  │  4. GitHub App bot creates PR (bot identity, not you)
  │  5. PAT (OMOQ_SYNC_TOKEN) approves PR (different identity, satisfies review)
  │  6. Auto-merge enabled
  │
  │  7. CI fires: standalone build on ubuntu + macOS (PR event)
  │     PASS → auto-merge fires → merged to openmoq-main
  │     FAIL → PR stays open, Slack alert, developer resolves
  │
  ▼
openmoq-main (merge lands)
  │  8. Push to openmoq-main triggers publish-artifacts
  │  9. Standalone build on all 4 platforms
  │ 10. Release created: build-<sha> with per-platform tarballs
  │
  ▼
orelay submodule can point to this commit
```

### Developer workflow

Normal trunk-based git. Developers open PRs against `openmoq-main`. CI runs
standalone build. Reviews + merge as usual. Changes persist through upstream
syncs because git merge handles it — once a merge conflict is resolved, the
merge base advances and the same conflict doesn't recur.

Two classes of local mods:
- **Upstreamable**: commit on openmoq-main, PR upstream when ready. When upstream
  absorbs it, next sync merge resolves cleanly (or trivial conflict, resolved once).
- **Non-upstreamable**: permanent commits on openmoq-main. Git merge carries them
  forward naturally.

### What runs where

| Branch | Workflows | Trigger |
|--------|-----------|---------|
| `main` | **NOTHING** (all upstream workflows disabled) | — |
| PR → `openmoq-main` | `openmoq-ci.yml` (standalone build) | `pull_request` |
| `openmoq-main` (push) | `openmoq-publish-artifacts.yml` | `push` |
| `openmoq-main` (cron) | `openmoq-upstream-sync.yml` | `schedule` / `workflow_dispatch` |

---

## Implementation

### Phase 1: Setup (do first, one-time)

**1a. Create GitHub App** (manual, in GitHub UI)
- Go to `https://github.com/organizations/openmoq/settings/apps/new`
- Name: `openmoq-sync-bot` (or similar)
- Permissions: Contents R/W, Pull requests R/W, Workflows R/W
- Webhook: off
- Install on `openmoq/moxygen` only
- Store: `OMOQ_APP_ID` (variable) + `OMOQ_APP_PRIVATE_KEY` (secret)

**1b. Create branches and set default**
```bash
git fetch upstream main
# Create openmoq-main from current main (has all openmoq files)
git checkout -b openmoq-main origin/main
git push -u origin openmoq-main
# Set as default
gh api repos/openmoq/moxygen -X PATCH -f default_branch=openmoq-main
# Reset main to pure upstream
git push origin upstream/main:refs/heads/main --force-with-lease
```

**1c. Branch protection**
- Remove protection from `main` (bot pushes directly)
- Add protection on `openmoq-main` (required check: standalone build, 1 review)

**1d. Disable upstream workflows** via API
- getdeps_linux, getdeps_mac, standalone, docker-build-amd64, docker-build-arm64,
  docker-interop-client — all disabled

**Verification**: `main` = upstream exactly. `openmoq-main` = default with all
openmoq files. No upstream workflows fire on push to main.

### Phase 2: Sync workflow (rewrite)

`.github/workflows/openmoq-upstream-sync.yml` on `openmoq-main`:

1. Generate app token (`actions/create-github-app-token@v2`)
2. Checkout `openmoq-main` with app token (for push access)
3. Scan upstream for newest green commit (reuse existing logic)
4. Fast-forward `main`: `git push origin $SHA:refs/heads/main`
5. Check if merge needed (`merge-base --is-ancestor`)
6. Create PR `main` → `openmoq-main` using app token (bot identity)
7. Approve PR using `OMOQ_SYNC_TOKEN` (your identity, satisfies review)
8. Enable auto-merge using app token
9. Slack on failure

Key simplifications vs current:
- No candidate branches, no patches, no conflict auto-resolution
- PR is `main` → `openmoq-main` (if already open, it auto-updates on next push)

### Phase 3: CI + artifact publishing (rewrite)

**`openmoq-ci.yml`** (new) — runs on PRs to `openmoq-main`:
- Standalone build on ubuntu-22.04 + macos-15
- `cmake -B _build -S standalone && cmake --build _build && ctest`

**`openmoq-publish-artifacts.yml`** (rewrite) — runs on push to `openmoq-main`:
- 4-platform matrix (ubuntu, macos, bookworm-amd64, bookworm-arm64)
- `cmake -B _build -S standalone -DCMAKE_INSTALL_PREFIX=install/`
- `cmake --build _build && cmake --install _build`
- Package: gather install prefix + any needed headers into tarball
- Release: `create-release.sh` (unchanged)

**Headers**: `cmake --install` should install headers for all deps (each
FetchContent dep has its own install rules). If moxygen's own headers aren't
covered, the packaging script gathers them from source. This is a packaging
concern — we verify during Phase 3 and handle in the collection script.

**New `collect-artifacts-standalone.sh`**: takes `--install-prefix` and
`--src-dir`, strips debug symbols, packages tarball. Much simpler than current
getdeps-based version.

### Phase 4: Cleanup

- Delete `openmoq/patches/` (no longer needed)
- Delete/archive old `collect-artifacts.sh`
- Update `openmoq/README.md` and `GITHUB_WORKFLOW.md`
- Clean up stale branches
- Update o-rly `.gitmodules` to track `openmoq-main`

### Phase 5: End-to-end verification

1. Trigger sync → main advances → PR created (bot) → approved (you) → CI passes → merged
2. Publish-artifacts fires → standalone build → tarball ~50-150 MB → release
3. Conflict test: local mod on openmoq-main + upstream touches same file → PR open → Slack → resolve

---

## Files

| File | Action | Phase |
|------|--------|-------|
| `.github/workflows/openmoq-upstream-sync.yml` | Rewrite for two-branch + app token | 2 |
| `.github/workflows/openmoq-ci.yml` | Create (standalone build CI) | 3 |
| `.github/workflows/openmoq-publish-artifacts.yml` | Rewrite for standalone build | 3 |
| `openmoq/scripts/collect-artifacts-standalone.sh` | Create | 3 |
| `openmoq/scripts/collect-artifacts.sh` | Delete | 4 |
| `openmoq/patches/` | Delete directory | 4 |
| `openmoq/README.md` | Update | 4 |
| `o-rly/design/GITHUB_WORKFLOW.md` | Update | 4 |
| `o-rly/.gitmodules` | Track `openmoq-main` | 4 |
