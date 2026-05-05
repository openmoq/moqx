# Release Process

This document describes the moqx release branch model, how release artifacts are produced, and how to cut a new release branch.

For the underlying CI workflow reference, see [ci-architecture.md](ci-architecture.md). For day-to-day contributor workflow, see [../CONTRIBUTING.md](../CONTRIBUTING.md).

## Branch Model

| Branch | moxygen pin | Build target | Audience |
|---|---|---|---|
| `main` | floats — uses moxygen `snapshot-latest` via `--use-latest` | `ghcr.io/openmoq/moqx:main-latest` (also `:latest` alias) + `snapshot-latest` GitHub release | Continuous integration; auto-deployed to `moqx-main.ci.openmoq.org` |
| `release/<name>` | **pinned** to a specific moxygen tag via [`.moxygen-release`](#moxygen-release-pin) | `ghcr.io/openmoq/moqx:<name>-latest` + `snapshot-<name>-latest` GitHub release | Demo / customer / event branches; manually deployed |
| `devops/*`, `feat/*`, etc. | follows the branch they were cut from | (no `ci main` — only `ci pr` runs on PRs) | Working branches |

`main` floats forward; `release/*` pins for reproducibility.

## moxygen-release pin

Release branches contain a top-level `.moxygen-release` file with a single line — the moxygen release tag (e.g. `v0.1.2`) the branch builds against.

**This file is load-bearing**; do not remove it from a release branch.

The contract is enforced in [`.github/workflows/ci-main.yml`](../.github/workflows/ci-main.yml) and [`ci-pr.yml`](../.github/workflows/ci-pr.yml):

```bash
# Release branches pin a specific moxygen release tag via
# .moxygen-release. Main uses snapshot-latest via --use-latest.
if [ -f .moxygen-release ]; then
  export MOQX_MOXYGEN_RELEASE_TAG=$(cat .moxygen-release | tr -d '[:space:]')
  bash scripts/build.sh setup --no-fallback
else
  bash scripts/build.sh setup --no-fallback --use-latest
fi
```

The `deps/moxygen` submodule must point at the same commit the tag resolves to. CI does not cross-check, but a divergence will cause the build to download the wrong tarball.

When merging `main` into a `release/*` branch, the `.moxygen-release` modify-vs-delete conflict resolves **in favor of keeping the file**, then bump it to match the merged submodule pin.

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
2. **Add the moxygen pin.** Pick the moxygen release tag your demo will run against (typically the latest `vX.Y.Z` whose hash is already on `main`):
   ```bash
   echo "v0.1.2" > .moxygen-release
   git add .moxygen-release
   git commit -m "release/<name>: pin moxygen v0.1.2"
   git push origin release/<name>
   ```
3. **Verify CI.** Push triggers `ci main`. Confirm `snapshot-<name>-latest` and `ghcr.io/openmoq/moqx:<name>-latest` were produced successfully.
4. **(Optional) Manually deploy** via the `deploy relay` workflow (`workflow_dispatch`), passing the release branch as the ref. The default deploy target is `moqx-<name>.ci.openmoq.org`.

## Updating a Release Branch (sync from main)

When a release branch needs to absorb fixes from `main`:

1. Verify the moxygen tag you want to land on is already tagged (`vX.Y.Z`) and `main` builds against it cleanly. Pinning at a tag main has continuously CI-tested derisks the merge.
2. Open a PR from a `devops/<branch>-vX.Y.Z` working branch into the release branch:
   ```bash
   git checkout -b devops/<release>-vX.Y.Z origin/release/<name>
   git merge origin/main         # resolve .moxygen-release in favor of keeping
   echo "vX.Y.Z" > .moxygen-release
   git add .moxygen-release && git commit --amend --no-edit
   git push origin devops/<release>-vX.Y.Z
   gh pr create --base release/<name> --head devops/<release>-vX.Y.Z ...
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
