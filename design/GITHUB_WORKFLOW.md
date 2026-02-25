# OpenMOQ GitHub Workflow

## Goals

1. **Always-green moxygen fork** — `openmoq/moxygen` tracks upstream `facebookexperimental/moxygen` automatically, applying local patches and validating before merge.
2. **Fast o-rly CI** — Pre-built per-platform moxygen artifact bundles eliminate 30–60 min dependency builds from o-rly CI.
3. **Minimal manual intervention** — Developers are only paged when something breaks. Routine syncs are silent.

---

## 1. Moxygen Fork (`openmoq/moxygen`)

### 1.1 Fork Structure

The fork mirrors upstream with one addition: a top-level `openmoq/` directory for local customizations (patches, scripts). OpenMOQ-specific workflows are prefixed `openmoq-` to avoid collisions on upstream sync.

Key paths:
- `openmoq/patches/` — Ordered customizations applied during sync (see §2)
- `openmoq/scripts/` — Locally-testable CI scripts
- `.github/workflows/openmoq-*.yml` — OpenMOQ workflows (not upstream)

### 1.2 Upstream Sync (`openmoq-upstream-sync.yml`)

Runs daily (cron) and on manual dispatch. Steps:

1. **Find newest green upstream commit** — Scans the last 20 commits on `facebookexperimental/moxygen:main` (newest first) and picks the most recent one with passing GitHub Actions CI. Skips commits already integrated or with existing candidate branches.
2. **Create candidate branch** — `sync-upstream-<sha>`, merge upstream via `git merge --no-ff`.
3. **Apply patches** — Processes `openmoq/patches/` entries in order (see §2).
4. **Open PR** — PR from candidate branch → `main`. Uses REST API (GraphQL `createPullRequest` is blocked on GitHub fork repos).
5. **Auto-merge** — On CI pass, the PR auto-merges. No manual approval needed because o-rly pins moxygen via submodule SHA.

**When it breaks:** If patches fail or CI fails, the candidate branch/PR persists for manual resolution. A Slack alert fires (see §5).

### 1.3 Artifact Publishing (`openmoq-publish-artifacts.yml`)

Triggers on merge to `main`. Builds moxygen + all Meta deps from source (no system packages) on 4 targets:

| Artifact | Environment | Notes |
|---|---|---|
| `moxygen-ubuntu-22.04-x86_64.tar.gz` | Ubuntu 22.04 runner | Primary native Linux |
| `moxygen-macos-15-arm64.tar.gz` | macOS 15 runner | Requires Xcode 16 (C++20 P0960R3) |
| `moxygen-bookworm-amd64.tar.gz` | `debian:bookworm` container | Docker AMD64 |
| `moxygen-bookworm-arm64.tar.gz` | `debian:bookworm` container on ARM64 | Docker ARM64 |

Artifacts are published as GitHub Release assets tagged `build-<sha>`. Old releases auto-pruned (keep 20).

The `collect-artifacts.sh` script handles: build-tool exclusion (ninja, cmake, autoconf, etc.), debug symbol stripping + `.debug` sidecar extraction, and 2 GiB limit enforcement.

> **Open: Tarball size** — Linux tarballs are ~1.6 GB while macOS is ~108 MB. We are likely shipping more transitive deps than o-rly actually needs at link time. Investigate filtering to only the deps that appear in o-rly's link line. Alan's standalone-build PR (#99) will also significantly reduce this by using FetchContent with minimal deps.

---

## 2. Patch & Merge System

The `openmoq/patches/` directory carries forward local changes that would be overwritten on each upstream sync. Entries are processed **in alphabetical order** by the sync workflow.

### 2.1 Patch Files (`.patch`)

Standard `git format-patch` output, applied via `git am`. Three-way merge is attempted when clean apply fails. If the patch is fully absorbed upstream, it is silently skipped.

**Creating a patch:**

```bash
# 1. Make your change on a clean branch
git checkout -b my-fix main
# edit upstream-owned files (manifests, cmake, etc.)
git add build/fbcode_builder/manifests/some-dep
git commit -m "Fix some-dep for bookworm containers"

# 2. Generate and rename with the next index
git format-patch -1 HEAD -o openmoq/patches/
mv openmoq/patches/0001-*.patch openmoq/patches/002-fix-some-dep-bookworm.patch

# 3. Verify
git apply --check openmoq/patches/002-fix-some-dep-bookworm.patch

# 4. Commit the .patch file (not the direct change) to main
```

### 2.2 Branch Merges (`.merge`)

For changes too complex for a single patch, create a `.merge` file that references a branch. The sync workflow will `git merge --no-ff` the specified ref at that point in the sequence.

**Format** (`openmoq/patches/003-custom-transport.merge`):
```
# Branch merge directive — processed by openmoq-upstream-sync
# One line: <branch-or-ref> [<commit-sha>]
# If SHA is given, merge that exact commit. Otherwise merge branch tip.
openmoq/custom-transport abc1234def5678
```

**Creating a branch merge:**

```bash
# 1. Do your work on a branch in the openmoq/moxygen repo
git checkout -b openmoq/custom-transport main
# make complex multi-file changes, multiple commits, etc.
git push origin openmoq/custom-transport

# 2. Record the merge directive with the next index
echo "openmoq/custom-transport $(git rev-parse HEAD)" \
  > openmoq/patches/003-custom-transport.merge

# 3. Commit the .merge file to main
```

### 2.3 When to Use What

| Approach | Use for |
|---|---|
| **Patch** (`.patch`) | Small, focused changes to upstream-owned files (manifests, cmake modules) |
| **Branch merge** (`.merge`) | Multi-file or multi-commit changes that are hard to express as a single patch |
| **Direct commit** | Files owned by OpenMOQ (`openmoq/`, `openmoq-*.yml`, `docker/`) — not overwritten by sync |

### 2.4 Retiring Patches

When a patch is absorbed upstream, the sync workflow auto-skips it. Once confirmed skipped across two sync cycles, delete the `.patch` file. This is a manual housekeeping step.

> **DevOps note:** When a patch or merge fails during sync, the candidate PR will be created with a failure label and Slack alert. A developer must manually resolve conflicts on the candidate branch and push a fix.

---

## 3. o-rly Relay (`openmoq/o-rly`)

### 3.1 Moxygen Submodule

o-rly includes `openmoq/moxygen` as a Git submodule at `deps/moxygen`, pinned to a specific commit SHA on `main`. This gives deterministic builds — o-rly is never implicitly affected by changes to the moxygen fork.

### 3.2 Submodule Bump Workflow

An automated workflow opens a PR to bump the submodule to the latest `main` commit. If a bump PR already exists, it updates in place (at most one open at a time). The team merges these on their own cadence.

### 3.3 Consuming Artifacts

When moxygen artifacts are available for the pinned SHA, o-rly CI downloads pre-built bundles instead of building from source:

```yaml
- name: Download moxygen artifacts
  run: |
    MOXYGEN_SHA=$(git -C deps/moxygen rev-parse HEAD)
    gh release download "build-${MOXYGEN_SHA}" \
      --repo openmoq/moxygen \
      --pattern "moxygen-${{ matrix.artifact-suffix }}.tar.gz" \
      --dir .scratch/
    tar xzf .scratch/moxygen-*.tar.gz -C .scratch/
```

`CMAKE_PREFIX_PATH` points to `.scratch/` — the rest of the build is unchanged.

### 3.4 Build Targets

| Platform | Architecture | Status |
|---|---|---|
| Ubuntu 22.04 | x86_64 | Working |
| macOS 15 | ARM64 | Working |
| Docker bookworm AMD64 | x86_64 | Working |
| Docker bookworm ARM64 | ARM64 | Working |
| Rocky Linux 9 | x86_64 | TBD |

### 3.5 Docker Strategy

Two Dockerfiles:
- **`docker/Dockerfile`** — Full multi-stage build. Self-contained, builds entire Meta dep stack inside Docker. Used until per-arch artifacts are consumed.
- **`docker/Dockerfile.ci`** — Copies pre-built binary into minimal `debian:bookworm-slim`. Activates when o-rly CI consumes moxygen artifacts.

Images publish to **GHCR** (`ghcr.io/openmoq/o-rly`) on merge to `main`. Tags: `latest` + `sha-<7chars>`.

---

## 4. DevOps Attention Items

These steps require human intervention when they arise:

| Situation | Action |
|---|---|
| **Patch/merge fails during sync** | Resolve conflicts on the candidate branch, push fix, let CI re-run |
| **Upstream CI broken for all recent commits** | Sync auto-skips. Check upstream; if extended, may need to manually cherry-pick fixes |
| **Tarball exceeds 2 GiB** | Review deps in `collect-artifacts.sh` exclude list. Check if strip is working. Transient regressions upstream can add large new deps |
| **New platform target needed** | Add matrix entry to `openmoq-publish-artifacts.yml`, matching entry to o-rly CI |
| **Token rotation** | `OMOQ_SYNC_TOKEN` (fine-grained PAT) — used for push + auto-merge. `OMOQ_SLACK_WEBHOOK_URL` — Slack incoming webhook |
| **Retiring a patch** | Verify it's been auto-skipped on 2+ syncs, then delete the `.patch` file from `openmoq/patches/` |

---

## 5. Notifications

Slack channel: `#github-notifications` (via `OMOQ_SLACK_WEBHOOK_URL` incoming webhook).

Only essential alerts — routine successful operations are silent.

| Event | Notified? |
|---|---|
| Upstream sync candidate created | No |
| Upstream sync **failed** (patch conflict, CI failure) | **Yes** |
| Artifact build + release **succeeded** | **Yes** |
| Artifact build **failed** | **Yes** |

---

## 6. Open Items

| # | Item | Priority |
|---|---|---|
| 1 | Investigate tarball size (Linux ~1.6 GB vs macOS ~108 MB) | High |
| 2 | Implement `.merge` directive support in sync workflow | Medium |
| 3 | Add Rocky Linux 9 to build matrix | Medium |
| 4 | Wire o-rly CI to consume moxygen artifacts | High |
| 5 | Implement `Dockerfile.ci` path in o-rly | Medium |
| 6 | Define test coverage thresholds | Low |
| 7 | Upstream PR #99 (Alan's standalone build) — will replace getdeps | Tracking |
