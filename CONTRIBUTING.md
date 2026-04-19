# Contributing to moqx

Thanks for your interest in moqx. This doc covers how we develop,
review, and merge changes. For build instructions see
[BUILD.md](BUILD.md); for runtime see [RUNNING.md](RUNNING.md). For
architecture and planning material, see [docs/](docs/).

## Guiding principle

> Producing code in the era of AI is cheap. Reviewer attention is the
> scarce resource. We optimize our workflow for human review.

## Pull request scope

**One PR = one cohesive thesis.** A reviewer should be able to read
the title and predict the diff.

- ✅ "fix: guard against publisher reconnect during upstream subscribe"
- ❌ "various fixes and cleanups" (no thesis)
- ❌ "feature X + refactor Y + bump Z" (split it)

If you notice an unrelated fix while working, open a second PR.
Independent PRs land in parallel; bundled PRs stall on the slowest
reviewer.

## PR state

A PR that looks useful and has all checks green will be merged when a
maintainer is available. No extra nudge needed. Use PR state to signal
intent:

- **Draft** — not ready for review. Reviewers aren't auto-requested.
  CI still runs, so you can iterate on green checks before asking for
  review.
- **Ready** (non-draft, no `WIP:` prefix) — ready for review and
  ready to merge once review and CI pass.
- **`WIP:` prefix** in the title — ready for review and CI, but not
  yet ready for merge. Maintainers won't merge `WIP:` PRs regardless
  of check state.

## How to contribute

This is a public repository and accepts fork-based PRs from anyone.

- **Outside contributors**: fork the repo, push a branch, open a PR
  against `main`.
- **Org members**: push a feature branch directly to this repo, open a
  PR against `main`.

CI runs on every PR with no secrets exposed. Publish, release, and
deploy only run on `push: main` after merge.

## Reviews

At least one approving review is required before merge. The reviewer
pool is small — be patient, and reciprocate by reviewing others.

**Admin override** (`gh pr merge --admin`) is reserved for:
- CI/infrastructure repairs where branch protection itself is the block.
- Release-critical merges under demo-window urgency.
- Documentation-only changes when waiting costs more than reviewing.

Note the override in the PR description: `Admin override: <reason>`.

## CI

Two workflow files gate changes:

- `ci pr` — format, build (linux + asan debug), tests. Must pass before merge.
- `ci main` — publish / release / deploy on push to `main` and `release/*`.

See [docs/ci-architecture.md](docs/ci-architecture.md).

## Branches

- `main` — rolling head; `snapshot-latest` image builds from here.
- `release/<name>` — pinned demo / customer release branches. See
  [docs/release.md](docs/release.md).
- `devops/*`, `feature/*`, `fix/*`, `hotfix/*` — convention only, no enforcement.

## Merge

PRs are squash-merged; the PR title becomes the commit message on
`main`, so write titles that summarize the change well. Branch commit
organization (rebase, amend, multiple commits) is up to the author —
it has no effect on the merged result.

If preserving multiple commits aids review or `git blame` (e.g., a
refactor followed by a correctness fix you want to bisect separately),
say so in the PR description and use a merge commit instead.

## Local development

Before submitting a PR:

1. `clang-format-19 -i` changed C++ files (CI checks this).
2. Build and run the test suite locally — faster feedback than CI.
3. Update [docs/config.md](docs/config.md) or [RUNNING.md](RUNNING.md)
   if you changed admin API or runtime config.
4. Add tests for new behavior; bug fixes include a regression test.

## Issues

GitHub Issues track bugs and feature work. Provide enough context —
version, config, repro steps, logs — for a maintainer to reproduce.

## Security & License

Report security issues via [SECURITY.md](SECURITY.md). Do not file
public issues for security reports. By contributing to moqx, you agree
your contributions are licensed under the project's [LICENSE](LICENSE).
