# Release Process

This document describes the moqx release branch model, how release artifacts are produced, and how to cut a new release branch.

For the underlying CI workflow reference, see [ci-architecture.md](ci-architecture.md). For day-to-day contributor workflow, see [../CONTRIBUTING.md](../CONTRIBUTING.md).

## Branch Model

| Branch | moxygen pin | Build target | Audience |
|---|---|---|---|
| `main` | floats — `MOXYGEN_REV` bumped daily by the sync bot | `ghcr.io/openmoq/moqx:main-latest` (also `:latest` alias) + `snapshot-latest` GitHub release | Continuous integration; auto-deployed to `moqx-main.ci.openmoq.org` |
| `release/<name>` | **pinned** — `MOXYGEN_REV` frozen (bot doesn't touch release branches) | `ghcr.io/openmoq/moqx:<name>-latest` + `snapshot-<name>-latest` GitHub release | Demo / customer / event branches; manually deployed |
| `devops/*`, `feat/*`, etc. | follows the branch they were cut from | (no `ci main` — only `ci pr` runs on PRs) | Working branches |

`main` floats forward; `release/*` pins for reproducibility.

## Pinning moxygen on a release branch

The pin lives in [`cmake/dependencies.cmake`](../cmake/dependencies.cmake) — the
`MOXYGEN_REV` line. It is per-branch by construction: a `release/*` branch simply
commits the `MOXYGEN_REV` it should build against, and the sync bot (which only
targets `main`) never advances it.

The prebuilt fetch resolves the pin to a release tag itself and verifies that
release's commit against it, so a frozen `MOXYGEN_REV` is all a release branch
needs — provided a tag stays retained at that rev.

**Freeze on a `v*`-tagged moxygen rev.** Between moxygen releases `main` carries
revs whose only tag is the rolling `snapshot-latest`. That tag moves on the next
publish and takes the release's assets with it, and every `--mode prebuilt` build
of the frozen branch then fails at configure. Check a candidate with
`git ls-remote --tags https://github.com/openmoq/moxygen | grep <sha>`, or accept
that the branch is `--mode from-source` only.

Optionally pin the exact tag to fetch from by setting `MOXYGEN_RELEASE_TAG` in the
same file — useful when a commit carries several tags and you want a specific one:

```cmake
set(MOXYGEN_RELEASE_TAG "<tag>")  # a tag on openmoq/moxygen *releases*
```

The tag must be one of [openmoq/moxygen's releases](https://github.com/openmoq/moxygen/releases)
— moqx's own `snapshot-<name>-latest` releases are a different repo. Its commit is
verified against `MOXYGEN_REV`, so an inconsistent pin fails the configure.

When merging `main` into a `release/*` branch, resolve the `cmake/dependencies.cmake`
conflict in favor of the release branch's pinned `MOXYGEN_REV`.

## Snapshot Releases

Every push to `main` or `release/*` produces a rolling pre-release on GitHub:

- `main` → `snapshot-latest`
- `release/<name>` → `snapshot-<name>-latest`

Each push deletes and recreates the release at the new commit, so the tag always points at the most recent build. Releases are marked `--prerelease` and contain the binary tarball (`moqx-bookworm-amd64.tar.gz`).

## Docker Image Tags

Per-build images are pushed to `ghcr.io/openmoq/moqx`:

| Tag | Contents | Lifecycle |
|---|---|---|
| `<short-sha>` | Exact build | Permanent |
| `main-latest` | Most recent main build | Replaced on every push to `main` |
| `latest` | Same as `main-latest` (back-compat alias) | Replaced on every push to `main` |
| `<release-name>-latest` | Most recent build of `release/<name>` | Replaced on every push to that release branch |

The interop client image (`ghcr.io/openmoq/moqx-interop-client`) follows the same tagging scheme.

## Cutting a New Release Branch

To open a new demo / customer release branch:

1. **Create the branch from main** at the desired starting commit:
   ```bash
   git checkout -b release/<name> main
   ```
2. **Freeze the moxygen pin.** Set `MOXYGEN_REV` (and optionally `MOXYGEN_RELEASE_TAG`)
   in [`cmake/dependencies.cmake`](../cmake/dependencies.cmake) to the commit your
   demo runs against — a `v*`-tagged moxygen rev, per the note above, not
   necessarily the current `main` value:
   ```bash
   # edit cmake/dependencies.cmake: MOXYGEN_REV "<sha>"
   git commit -am "release/<name>: pin moxygen <sha>"
   git push origin release/<name>
   ```
3. **Verify CI.** Push triggers `ci main`. Confirm `snapshot-<name>-latest` and `ghcr.io/openmoq/moqx:<name>-latest` were produced successfully.
4. **(Optional) Manually deploy** via the `deploy relay` workflow (`workflow_dispatch`), passing the release branch as the ref. The default deploy target is `moqx-<name>.ci.openmoq.org`.

## Updating a Release Branch (sync from main)

When a release branch needs to absorb fixes from `main`:

1. Verify the moxygen commit you want to land on is one `main` has continuously CI-tested; pinning there derisks the merge.
2. Open a PR from a `devops/<branch>-<sha>` working branch into the release branch:
   ```bash
   git checkout -b devops/<release>-<sha> origin/release/<name>
   git merge origin/main         # resolve cmake/dependencies.cmake to the intended MOXYGEN_REV
   git push origin devops/<release>-<sha>
   gh pr create --base release/<name> --head devops/<release>-<sha> ...
   ```
3. **Use a merge commit (not squash)** when merging into a release branch — preserves the upstream commits' attribution and history on the release branch.
4. (Recommended for first-time release branches) **dry-run** by pushing the merged commit as `release/<name>-test` first, watch a full `ci main` cycle end-to-end, then delete the throwaway branch + its `snapshot-<name>-test-latest` release + Docker tags before merging the real PR.

## Tagged Releases

> TODO (Alan): formal version tagging policy. Today, snapshot releases are the only published artifact. A tagged `vX.Y.Z` release flow (analogous to moxygen's `version release` workflow) is planned — see [issue TBD].

## Deploy

`main` auto-deploys to `moqx-main.ci.openmoq.org` after every successful `ci main` run.

Release branches are deployed manually via the `deploy relay` workflow (`workflow_dispatch`):

- Default domain: `moqx-<release-name>.ci.openmoq.org`
- DNS A record auto-provisioned via Route53
- TLS cert auto-provisioned/renewed via certbot + Route53 DNS challenge

Inputs to `deploy relay` (all optional, branch-derived defaults):

- `image_tag` — defaults to `<branch-label>-latest`
- `domain` — defaults to `moqx-<branch-label>.ci.openmoq.org`
- `restart_only` — restart the existing image without redeploying
- `verbose` — GLOG verbosity level

See [ci-architecture.md](ci-architecture.md) for the underlying workflow details.
