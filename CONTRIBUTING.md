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

- **Draft** — not ready for review; CI still runs.
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

- `ci pr` — format, build (linux + asan), tests. Must pass before merge.
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

## Changelog

[CHANGELOG.md](CHANGELOG.md) follows [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/). Versions follow [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html).

- A PR adds its user-visible changes to the `[Unreleased]` section, in the same PR.
- An entry covers what an operator or client of the shipped product can observe.
- A headline feature goes directly under the `[Unreleased]` heading, above the subsections.
- Everything else goes under the matching subheading: `Added`, `Changed`, `Deprecated`, `Removed`, `Fixed` or `Security`. Create the subheading if it is missing.
- Write for the operator, not the reviewer: what changed for them, in one sentence. Name the config key, flag or endpoint.
- Link the doc section that covers the change, when one exists.
- A breaking change starts with `**Breaking:**`.
- Keep the file's style, including:
  - version headings in setext form, underlined with `===`;
  - full PR links: `([#123](https://github.com/openmoq/moqx/pull/123))`.

Cutting a release: see [docs/release.md](docs/release.md#changelog).

## Local development

Before submitting:

1. `scripts/dev/format.sh` — formats and lints C++ and Python. Needs
   clang-format 19 and either `uv` or the pinned ruff on PATH.
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

Every C/C++ source file starts with one of two headers. New files use:

```cpp
/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */
```

Files copied or substantially derived from moxygen keep Meta's copyright:

```cpp
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Copyright (c) OpenMOQ contributors.
 * Originally from github.com/facebookexperimental/moxygen.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */
```

Put a file description in its own comment below the header.
