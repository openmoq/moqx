# CI and Automation

This document describes the CI pipelines, upstream sync, submodule management,
artifact publishing, and relay deployment across the openmoq organization.

## Cross-Repo Dependency

```
┌─────────────────────────────────────────────────────────────┐
│  moxygen (library)                                          │
│                                                             │
│  ci main ──► snapshot-latest release (tarballs, all platforms)
│                         │                                   │
└─────────────────────────┼───────────────────────────────────┘
                          │  submodule SHA pins which tarball
                          ▼
┌─────────────────────────────────────────────────────────────┐
│  moqx (application)                                        │
│                                                             │
│  ci main ──► Docker image (ghcr.io/openmoq/moqx)          │
│              + binary tarball                                │
│              + auto-deploy to moqx-000                       │
└─────────────────────────────────────────────────────────────┘
```

moqx's `deps/moxygen` submodule pins a moxygen commit. `setup-deps-release.sh`
downloads the matching pre-built tarball from moxygen's release.

## End-to-End Flow

```
facebookexperimental/moxygen  (upstream commit lands)
        │
        ▼  daily 07:00 UTC
   upstream sync creates sync/<sha> PR on openmoq/moxygen
        │
        ▼  ci pr runs (~28 min)
   sync auto-merge merges PR
        │
        ▼  push to main triggers ci main (~40 min)
   snapshot-latest updated with new tarballs
        │
        ▼  repository_dispatch (automatic)
   moxygen sync creates sync-moxygen/<sha> PR on moqx
        │
        ▼  ci pr runs (~5 min)
   moxygen auto-merge merges PR
        │
        ▼  push to main triggers ci main (~10 min)
   Docker image pushed to GHCR, binary tarball released
        │
        ▼  deploy job (automatic)
   moqx-000.ci.openmoq.org updated with new image
```

The full chain from upstream commit to deployed relay is fully automated.

---

## moxygen Workflows

### 1. `ci pr` — Pull request verification

**Trigger:** PR to main | **Time:** ~28 min

| Job | Runner | Purpose |
|-----|--------|---------|
| linux | ubuntu-22.04 | Build + test |
| macos | macos-15 | Build + test |
| pico (linux) | ubuntu-22.04 | Picoquic transport build |
| pico (macos) | macos-15 | Picoquic transport build |
| asan debug | ubuntu-22.04 | ASAN/UBSAN build + test |

Required checks: `linux`, `macos`.

### 2. `ci main` — Build, Publish, Release, Notify

**Trigger:** push to main | **Time:** ~40 min

```
build (5 jobs) ──► publish (4 platforms) ──► release ──► notify + dispatch to moqx
```

Publish produces per-platform tarballs (ubuntu, bookworm, macos) + debug symbol archives.
Release creates/updates `snapshot-latest` pre-release.
Notify dispatches `moxygen-update` to moqx via `repository_dispatch`.

### 3. `upstream sync` — Daily upstream mirror

**Trigger:** daily 07:00 UTC + manual | **Time:** <1 min

- Scans last 20 upstream commits, picks newest with green CI
- Advances `origin/upstream` branch (fast-forward only)
- Creates `sync/<sha>` PR if upstream is ahead of main
- Blocks if an existing sync PR is open (one at a time)
- Notifies Slack/email on conflicts, failures, or all-red upstream

### 4. `sync auto-merge` — Merge green sync PRs

**Trigger:** `ci pr` completes on `sync/*` branch | **Time:** <1 min

Merges the sync PR if CI passed. Deletes the sync branch after merge.

### 5. `version release` — Manual tagged release

**Trigger:** manual (version input) | **Time:** <1 min

Promotes `snapshot-latest` artifacts to a versioned `vX.Y.Z` release (no rebuild).

---

## moqx Workflows

### 1. `ci pr` — Pull request verification

**Trigger:** PR to main | **Time:** ~5 min

| Job | Runner | Purpose |
|-----|--------|---------|
| check-format | ubuntu-latest (trixie) | clang-format-19 check |
| linux | ubuntu-22.04 | Build + test (from-release tarball) |
| asan debug | self-hosted (linode) | ASAN/UBSAN build + test |

Format check must pass before build runs.

### 2. `ci main` — Build, Publish, Release, Deploy, Notify

**Trigger:** push to main | **Time:** ~10 min

```
check-format + build ──► publish (Docker) ──► release ──► deploy (moqx-000) ──► notify
```

- Publish builds a multi-stage Docker image (bookworm), runs smoke test, pushes to GHCR
- Release creates/updates `snapshot-latest` with binary tarball
- Deploy automatically updates moqx-000.ci.openmoq.org with the new image
- Notify sends Slack + email with per-job status

### 3. `moxygen sync` — Automated submodule update

**Trigger:** `repository_dispatch` from moxygen + manual | **Time:** <1 min

- Checks for blocking `sync-moxygen/*` PR (one at a time)
- Updates `deps/moxygen` submodule to dispatched SHA
- Creates PR with dual identity (bot creates, PAT approves)
- Notifies on block or failure

### 4. `moxygen auto-merge` — Merge green sync PRs

**Trigger:** `ci pr` completes on `sync-moxygen/*` branch | **Time:** <1 min

Merges the sync PR if CI passed. Deletes the branch after merge.

### 5. `deploy relay` — Manual deployment

**Trigger:** manual `workflow_dispatch` | **Runner:** self-hosted (linode)

Deploys a specific image tag to a named instance. Handles TLS cert
provisioning/renewal via Route53, health check verification, Slack notification.

Reserved instances:
- `moqx-000` — CI auto-deploy target (updated automatically by `ci main`)

Developer instances (`moqx-001+`) can be added to the instance list for manual
testing deployments.

---

## Two-Branch Architecture (moxygen)

openmoq/moxygen uses a two-branch model:

- **`upstream`** — Pure mirror of `facebookexperimental/moxygen`. Updated by the
  sync workflow. No openmoq files.
- **`main`** (default) — Working branch with all openmoq customizations.
  Developer PRs, artifacts, and releases come from here.

The sync workflow creates PRs from `upstream` → `main`. Git merge handles
conflict resolution: once a conflict is resolved, the merge base advances and
the same conflict doesn't recur.

### Dual Identity for Sync PRs

Both repos use two identities to satisfy required review rules:

- **GitHub App** (`omoq-sync-bot`): creates the PR and pushes the branch
- **PAT** (`OMOQ_SYNC_TOKEN`): approves the PR (different identity from author)

---

## Linkage Profile

The build uses a hybrid static/dynamic strategy. Meta's libraries are statically
linked; common system libraries stay dynamic.

**Static (all platforms):** folly, fizz, wangle, mvfst, proxygen, fmt, boost

**Static (Linux only):** sodium, zlib

**Dynamic (all platforms):** OpenSSL, glog, gflags, double-conversion, libevent,
zstd, libunwind, libc, libstdc++

---

## Cost Summary (GitHub-hosted minutes per event)

Based on recent runs (March 2026):

| Event | Wall clock | Approx billed minutes |
|-------|-----------|----------------------|
| moxygen PR | ~28 min | ~55 (+ macos 10x multiplier) |
| moxygen main push | ~40 min | ~75 (3-platform publish) |
| moqx PR | ~5 min | ~10 |
| moqx main push | ~11 min | ~15 |

All build jobs use ccache:

- **~35-50%** hit rate after upstream sync (many changed files)
- **~65-80%** on incremental builds (typical PR or small change)
- Warm cache cuts moxygen build time roughly in half
