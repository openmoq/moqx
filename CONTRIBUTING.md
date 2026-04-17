# Contributing to moqx

Thanks for your interest in moqx. This doc covers how we develop, review, and merge changes. For build instructions see [BUILD.md](BUILD.md); for runtime see [RUNNING.md](RUNNING.md); for the CI/automation reference see [docs/ci-architecture.md](docs/ci-architecture.md); for the release branch model see [docs/release.md](docs/release.md).

## Guiding principle

> **Producing code in the era of AI is cheap. Reviewer attention is the scarce resource.** We optimize our workflow for easiest human review.

Everything below follows from that.

## Pull Request Scope

**One PR = one cohesive thesis.** A reviewer should be able to read the title and predict the diff. Examples:

- ✅ "fix: guard against publisher reconnect during upstream subscribe"
- ✅ "ci-main: make Package + release notes branch-symmetric"
- ❌ "various fixes and cleanups" (no thesis)
- ❌ "implement feature X + refactor Y + bump Z" (multiple theses → split)

If you're tempted to add an unrelated change because "it's a small fix and I'm here anyway," open a second PR. Independent PRs land in parallel; bundled PRs stall on the slowest reviewer.

### One PR = one commit (target)

Authors should land changes as a single squashed commit on `main`. We do **not** preserve dev-time commit noise (typo fixes, "address review", WIP) in the merged history.

- **Author squashes before merge.** Either rebase locally or use the GitHub "Squash and merge" button. The squashed commit message should match the PR title and reference the issue/PR.
- **Reviewable.io supports per-commit review** — feel free to keep dev commits during review for easier diffing, then squash at merge time.
- **Stacks** of related PRs are encouraged when work is logically separable. [Sapling](https://sapling-scm.com/) makes managing stacks much easier than `git rebase -i`.
- **Feature branches** are fine when a stack of PRs needs to land coherently before promoting to `main` — open PRs against the feature branch, then a final PR from feature-branch → `main`.

### Flexibility

These are defaults, not laws. If preserving multiple commits genuinely aids review or `git blame` (e.g., a refactor + correctness fix that you want to bisect separately), say so in the PR description and use a merge commit. Be ready to defend the choice.

## Reviews

- All PRs require at least one approving review before merge.
- The reviewer pool is small — be patient, and reciprocate by reviewing others.
- For trivial CI/docs/dependency-bump PRs, self-merge with admin override is acceptable when no reviewer is available within the urgency window. **Note the override in the PR description.**

### Admin overrides

Branch protection on `main` and `release/*` requires reviews. Admin override (`gh pr merge --admin`) is reserved for:

- **CI/infrastructure repairs** where the protection itself is what's blocking the fix.
- **Release-critical merges** under demo-window urgency when the reviewer pool is unreachable.
- **Documentation-only changes** where the cost of waiting outweighs the value of review.

Every admin override should be noted in the PR description (one line: "Admin override: <reason>") so it's auditable in `git log` and the merge UI.

## CI

Two workflow files gate all changes:

- **`ci pr`** runs on every PR — format check, build (linux + asan debug), tests. Must pass before merge.
- **`ci main`** runs on push to `main` and `release/*` — full publish/release/notify pipeline.

See [docs/ci-architecture.md](docs/ci-architecture.md) for the full workflow reference.

## Branches

- **`main`** — rolling head. Floats forward continuously via `snapshot-latest` builds.
- **`release/<name>`** — pinned demo / customer release branches. See [docs/release.md](docs/release.md).
- **`devops/*`** — working branches for infra/CI/docs work (your namespace).
- **`feat/*`, `fix/*`, etc.** — convention only; no enforcement.

## Local development

See [BUILD.md](BUILD.md) for build instructions and [RUNNING.md](RUNNING.md) for running the relay locally.

Before submitting a PR:

1. `clang-format-19 -i` your changed C++ files (CI checks this).
2. Build and run the test suite locally — `ci pr` will catch you, but local feedback is faster.
3. If you changed the admin API or runtime config, update [docs/config.md](docs/config.md) and/or [RUNNING.md](RUNNING.md).
4. Write tests for new behavior. Bug fixes should include a regression test.

## Issues

GitHub Issues track bugs and feature work. Provide enough context (version, config, repro steps, logs) for a maintainer to reproduce.

## License

By contributing to moqx, you agree your contributions are licensed under the project's [LICENSE](LICENSE).
