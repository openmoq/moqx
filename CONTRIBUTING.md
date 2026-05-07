# Contributing to moqx

This document describes current guidelines for contributing to the `moqx` project, and related OpenMOQ repos. Openness and community participation are foundational principles of the OpenMOQ Consortium. All are welcome and encouraged to use the software here freely, and contribute to testing, fixing, and enhancing the code when possible.

The following describes the general philosophy and approach for handling contributions. The process remains flexible and subject to review, and may change in the future. The ultimate goal is to facilitate the production of high-quality, high-performance, professional-grade software.

## Pull request scope and content

**Each PR should address a single and logically cohesive thesis.**

This makes reviews more approachable and more likely to occur. Avoid bundling various unrelated changes and ad hoc changes.

The following is a list of ideals for PR content — developer discretion and sound judgement is expected:

- The title should clearly reflect the functional impact of the PR (in most cases this becomes the commit message on the merge commit).
- The description should contain additional technical details and solution rationale.
- If the PR addresses an existing issue, reference it in the description — `Fixes #N` to auto-close on merge, or `Refs #N` for partial or related work.
- Tests or other evaluation criteria should be included for independent pass/fail evaluation (including unit tests where possible).
- Relevant logs, developer test output, or other supporting material may be attached to support the review process.

## PR state

PRs with all checks passing may be merged by maintainers at unpredictable times based on availability and relative priority. The author can signal merge intent in the following ways:

- **Draft** — not ready for review. No auto-reviewer request; CI still runs.
- **`WIP:` prefix** — ready for review and CI, not ready for merge.
- **Ready** (non-draft, no `WIP:` prefix) — merge when all checks pass (manual for now).

## How to contribute

`moqx` is a public repo — fork-based PRs are welcome.

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
- Docs-only or other low/no-risk changes.

## CI

- `ci pr` — format, build (linux + asan debug), tests. Must pass before merge.
- `ci main` — publish / release / deploy on push to `main` and `release/*`.

See [docs/ci-architecture.md](docs/ci-architecture.md).

## Branches

The following branch naming conventions are offered as developer guidance:

- `main` — rolling head; `snapshot-latest` builds from here.
- `release/<name>` — pinned demo / customer release branches. See [docs/release.md](docs/release.md).
- `devops/*`, `feature/*`, `fix/*`, `hotfix/*` — convention only, no enforcement.

The specific suffix branch name is up to the developer — something informative is often helpful (please clean up stale branches).

## Merge

PRs are squash-merged; the PR title becomes the commit message on `main`.
Authors are encouraged to maintain a concise, informative commit
history on the branch — it aids review. Authors may request a merge commit in the
PR description if preserving history on `main` is warranted.

> **Note:** *Delete branch on merge* is the current default setting.

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
