# Contributing to moqx

For build see [BUILD.md](BUILD.md); for runtime, [RUNNING.md](RUNNING.md);
for architecture and planning material, [docs/](docs/).

## Guiding principle

> Producing code in the era of AI is cheap. Reviewer attention is the
> scarce resource.

## Pull request scope

**One PR = one cohesive thesis.** A reviewer should read the title and
predict the diff.

- ✅ `fix: guard against publisher reconnect during upstream subscribe`
- ❌ `various fixes and cleanups`
- ❌ `feature X + refactor Y + bump Z` (split it)

Spot an unrelated fix while working? Open a second PR.

## PR state

Useful PRs with all checks green are merged when a maintainer is
available. Signal intent:

- **Draft** — not ready for review. No auto-reviewer request; CI still runs.
- **Ready** (non-draft, no `WIP:` prefix) — merge when green.
- **`WIP:` prefix** — ready for review and CI, not for merge.

## How to contribute

moqx is public — fork-based PRs welcome.

- Outside contributors: fork, branch, PR against `main`.
- Org members: branch directly on this repo, PR against `main`.

PRs run CI with no secrets. Publish, release, and deploy run only on
`push: main` after merge.

## Reviews

At least one approving review is required. The reviewer pool is small —
be patient, reciprocate.

**Admin override** (`gh pr merge --admin`) is for:
- CI/infrastructure repairs blocked by branch protection itself.
- Release-critical merges under urgency.
- Docs-only changes when waiting costs more than reviewing.

Note the override in the PR description: `Admin override: <reason>`.

## CI

- `ci pr` — format, build (linux + asan debug), tests. Must pass before merge.
- `ci main` — publish / release / deploy on push to `main` and `release/*`.

See [docs/ci-architecture.md](docs/ci-architecture.md).

## Branches

- `main` — rolling head; `snapshot-latest` builds from here.
- `release/<name>` — pinned demo / customer release branches. See
  [docs/release.md](docs/release.md).
- `devops/*`, `feature/*`, `fix/*`, `hotfix/*` — convention only, no enforcement.

## Merge

PRs are squash-merged; the PR title becomes the commit message on `main`.
Authors are encouraged to maintain a concise, informative commit
history on the branch — it aids review. Request a merge commit in the
PR description if preserving history on `main` is warranted.

## Local development

Before submitting:

1. `clang-format-19 -i` changed C++ files.
2. Build and run tests locally.
3. Update [docs/config.md](docs/config.md) or [RUNNING.md](RUNNING.md)
   if you changed admin API or runtime config.
4. Add tests; bug fixes include a regression test.

## Issues

GitHub Issues track bugs and features. Include version, config, repro
steps, logs.

## Security & License

Report security issues via [SECURITY.md](SECURITY.md) — not public
issues. Contributions are licensed under [LICENSE](LICENSE).
