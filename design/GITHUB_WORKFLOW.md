# OpenMOQ GitHub Development Environment

## Overview

This document describes the GitHub workflow architecture for the OpenMOQ relay development environment. It covers the upstream dependency management strategy for the moxygen library, the build and test pipeline for the o-rly relay repo, and the notification and monitoring integrations that keep the team informed of pipeline status.

### Notification Channels

All pipeline events, failures, and status updates across both the moxygen sync pipeline and the o-rly relay builds are sent to the following channels:

- **Email:** github-notifications@openmoq.org
- **Slack:** `#github-notifications` channel in the OpenMOQ Slack workspace

Notifications are kept concise — a short summary line with direct links to the relevant PR, workflow run, or test report. Details are accessed via the linked GitHub resources, not embedded in the notification body.

---

## 1. Upstream Moxygen Dependency Approach

### 1.1 Fork Strategy

OpenMOQ maintains a local "buffer" fork of the upstream [moxygen](https://github.com/facebookexperimental/moxygen) repository at [openmoq/moxygen](https://github.com/openmoq/moxygen). This fork serves as a consistently stable, tested snapshot of moxygen that includes any modifications required for OpenMOQ relay development. The fork is kept in sync with the upstream source via automated GitHub workflow pipelines.

### 1.2 Upstream Change Detection

The synchronization workflow monitors the upstream `facebookexperimental/moxygen` repository for new changes using the [GitHub Commits API](https://docs.github.com/en/rest/commits/statuses). The workflow runs on a **daily cron schedule** and checks the `HEAD` of the upstream `main` branch. Synchronization is triggered when the workflow detects a new commit where the combined commit status reports `state == success`, ensuring that only upstream commits that have passed their own CI checks are considered for integration.

**API Authentication:** The upstream `facebookexperimental/moxygen` repository is public, so the GitHub Commits API can be queried without authentication. However, unauthenticated requests are subject to a lower rate limit (60 requests/hour per IP). Given the daily polling cadence, this is not a concern. If needed in the future, a GitHub token can be added to increase the rate limit to 5,000 requests/hour.

### 1.3 Local Customization Directory

The `openmoq/moxygen` fork maintains a top-level `openmoq/` directory for all OpenMOQ-specific customizations that are not part of the upstream moxygen tree. This directory is organized by purpose:

- **`openmoq/patches/`** — Ordered patch files applied during the upstream sync workflow. Patches are named with a numeric prefix to enforce application order (e.g., `001-relay-config-hooks.patch`, `002-transport-buffer-fix.patch`). During the candidate branch workflow, patches are applied sequentially using `git apply` or `git am`.
- Additional subdirectories (e.g., scripts, configuration) may be added as needed for OpenMOQ-specific build or CI tooling that should live alongside the fork but outside the upstream source tree.

### 1.4 Candidate Branch and PR Workflow

When a qualifying upstream change is detected, the following automated sequence is executed:

1. **Branch creation:** A candidate branch named `sync-upstream-<sha>` is created in the `openmoq/moxygen` fork, where `<sha>` is the short hash of the upstream commit being integrated.
2. **Upstream merge:** The upstream changes are merged into the candidate branch using a **merge commit strategy** (`git merge --no-ff`). This preserves the full upstream commit history on the fork's `main` branch, making it straightforward to trace changes back to their upstream origin and to review what was introduced in each sync cycle. The merge commit itself serves as a clear marker of each synchronization point.
3. **Local patch application:** Patches from the `openmoq/patches/` directory are applied in order on top of the merged upstream changes.
4. **PR creation:** A pull request is opened from the candidate branch targeting the `main` branch of the fork.
5. **CI validation:** GitHub Actions workflows execute on the candidate branch to verify build stability and test passage.
6. **Auto-merge (on success):** Upon successful CI completion, the candidate branch is automatically merged into `main` with no manual approval required. This is safe because the downstream relay repo (`o-rly`) references the moxygen fork via a submodule pinned to a specific commit SHA — the relay is never implicitly affected by changes to the fork's `main` branch.
7. **Post-merge CI:** GitHub Actions run again on the updated `main` branch to re-confirm stability after merge.

**Conflict and failure handling:** If the upstream merge or patch application produces conflicts, or if CI fails on the candidate branch, the candidate branch and PR **persist for manual resolution**. The team is notified via the standard notification channels (see above) with a link to the failing PR. A developer must then manually resolve the conflict or patch failure, push a fix to the candidate branch, and allow CI to re-run.

### 1.5 Notification Events

The following pipeline events generate notifications to github-notifications@openmoq.org and the `#github-notifications` Slack channel:

| Event | Contents |
|---|---|
| Candidate branch/PR created | Link to PR, upstream commit SHA and message |
| Candidate PR build/test results | Pass/fail status, link to workflow run |
| Conflict or patch failure | Link to PR, description of failure point |
| Merge to main | Link to merge commit, upstream SHA integrated |
| Main branch build/test results | Pass/fail status, link to workflow run |

---

## 2. OpenMOQ Relay (o-rly) Build — GitHub Workflow Approach

### 2.1 Moxygen Submodule Integration

The OpenMOQ relay repository ([openmoq/o-rly](https://github.com/openmoq/o-rly)) includes the local moxygen fork ([openmoq/moxygen](https://github.com/openmoq/moxygen)) as a Git submodule, pinned to a specific commit SHA or tag on the fork's `main` branch. The submodule is located at `deps/` within the relay repository. The submodule target will always reference the `main` branch of the moxygen fork.

This approach provides the following guarantees:

- **Deterministic builds:** The relay always builds against a known, specific version of moxygen.
- **Stability:** The pinned commit has already passed through the full upstream sync and validation pipeline described in Section 1.
- **Proximity to upstream:** The stated goal is that the pinned commit is as close to the upstream `HEAD` as possible, while remaining fully validated.

### 2.2 Submodule Update Policy

Updates to the submodule pointer are treated like any other code change in the relay repo: a PR is opened, CI must pass across the full build and test matrix, and the PR is manually merged by a developer after review.

**Automated PR creation:** A helper workflow will be implemented to automatically open a PR that bumps the submodule to the latest validated commit on the moxygen fork's `main` branch. To prevent accumulation of stale bump PRs, the workflow will follow these rules:

- If an open submodule-bump PR already exists, the workflow will **update the existing PR** (force-push the branch to the new target commit) rather than creating a new one.
- The PR title and body are updated to reflect the new target commit.
- This ensures there is at most one open submodule-bump PR at any time.

The initial submodule acceptance cadence is ad hoc — the team merges bump PRs when ready. The automated PR creation simply ensures a ready-to-merge PR is always available when the team decides to update.

### 2.3 Submodule Update Notifications

Submodule-bump PR creation and CI results are sent to the same notification channels (github-notifications@openmoq.org and `#github-notifications` Slack channel) following the same concise format: status summary with links to the PR and workflow run.

---

## 3. GitHub Workflow Build Targets

### 3.1 Platform Matrix

The o-rly GitHub Actions workflows verify that a full local build can be executed across the following platforms and distributions:

| Platform / Distro | Architecture | Status | Notes |
|---|---|---|---|
| **Ubuntu 22.04.5** | x86_64 | ✅ Confirmed | Primary Linux development target |
| **macOS 15.7.2** | ARM64 (Apple Silicon) | ✅ Confirmed | Uses `macos-14` or later GitHub-hosted runners |
| **Rocky Linux 9** | x86_64 | 🔲 TBD | RHEL 9 compatibility target |
| **Docker (debian-slim) — AMD64** | x86_64 | ✅ Confirmed | Production container image |
| **Docker (debian-slim) — ARM64** | ARM64 | ✅ Confirmed | Production container image |

> **⚠️ Remaining Open Items:**
>
> - **Rocky Linux 9 timeline:** TBD. A containerized approach within GitHub Actions (`docker://rockylinux:9`) is the simplest path.
> - **Build toolchain requirements:** Document compiler (GCC/Clang version), CMake version, and other toolchain dependencies in the workflow or `BUILD.md`.

### 3.1.1 Docker Build Strategy (Decided)

Docker images are built via `docker buildx build --platform linux/amd64,linux/arm64` for multi-arch support using QEMU emulation. Two Dockerfiles are maintained:

- **`docker/Dockerfile`** — Full multi-stage build (4 stages: base deps → moxygen → o-rly build → minimal runtime). Used for both local development (`docker build`) and CI Docker image creation. Self-contained: builds the entire Meta dependency stack (folly, fizz, wangle, mvfst, proxygen, moxygen) inside Docker via `getdeps.py`. This is the only practical path for multi-arch images until pre-built cross-architecture artifacts are available.

- **`docker/Dockerfile.ci`** — Simplified runtime-only image. Copies a pre-built `o_rly` binary into a minimal `debian:bookworm-slim` image. **Activates when `openmoq/moxygen` CI publishes per-architecture artifact bundles** (see Section 6), at which point o-rly CI can produce per-arch Linux binaries natively and package them without rebuilding inside Docker.

Docker layer caching uses the GitHub Actions cache backend (`cache-from: type=gha, cache-to: type=gha,mode=max`) to avoid rebuilding expensive dependency stages on every run.

### 3.2 Container Registry (Decided)

Container images are published to **GitHub Container Registry (GHCR)** at `ghcr.io/openmoq/o-rly`:

- Natively integrated with GitHub Actions (authentication via `GITHUB_TOKEN`, no additional secrets required).
- Free for public packages.
- Supports multi-arch image manifests.
- Images are linked directly to the GitHub repository in the UI.

Images are pushed on merge to `main` only. Tags: `latest` and the short git SHA (`sha-<7chars>`).

---

## 4. GitHub Workflow Tests and Reporting

### 4.1 Test Execution Scope

The non-Docker platform/distro builds (Ubuntu, macOS, Rocky Linux) execute the full test suite, which includes:

- **Unit tests:** Validate individual components and modules in isolation.
- **System tests:** Validate end-to-end behavior and integration across components.
- **Benchmark tests (optional):** Capture performance data under consistent, controlled conditions to enable comparison across commits and support performance optimization efforts.

> **⚠️ Needs More Detail:**
>
> - **Docker test policy:** Consider running at minimum a smoke test or health check within the built container images to validate that the application starts and responds correctly in its production packaging.
> - **Test framework:** What test framework is being used (e.g., Google Test, Catch2)? How are tests discovered and executed?
> - **Benchmark infrastructure:** What benchmarking tool is used? Is there a baseline storage and comparison mechanism? Are benchmark regressions blocking, advisory, or informational?
> - **Flaky test handling:** What is the policy for flaky tests? Are there retry mechanisms or quarantine labels?

### 4.2 Test Reporting (Decided)

Test results are published within the GitHub Actions results area with the following requirements:

- **Clear pass/fail indicators** for each test case and the overall suite.
- **Full log output** for all executed tests, including stdout and stderr capture.
- **Relevant trace data** to support problem isolation and diagnosis of failures (e.g., stack traces, assertion messages, sanitizer output).

**Reporting format:** JUnit XML, generated natively by CTest via `ctest --output-junit test-results.xml` (available in CMake 3.21+, which ships with Ubuntu 22.04). Results are rendered in the GitHub PR checks UI using [`mikepenz/action-junit-report`](https://github.com/mikepenz/action-junit-report), providing per-test pass/fail visibility directly in pull requests.

Artifact retention uses the GitHub Actions default of 90 days.

### 4.3 Code Coverage (Decided)

Code coverage is tracked to measure test effectiveness and identify untested code paths. The toolchain is entirely self-hosted (no third-party services):

- **Instrumentation:** Compile with GCC coverage flags (`--coverage` / `-fprofile-arcs -ftest-coverage`) to produce `.gcno` and `.gcda` files during test execution.
- **Collection:** Use **gcov** to process the raw coverage data, then **lcov** to aggregate results and filter out system/third-party headers and dependencies.
- **Reporting:** Generate HTML coverage reports via `genhtml` (included with lcov). The HTML report is uploaded as a **GitHub Actions artifact** for direct inspection. No third-party coverage service is used — coverage infrastructure will be implemented within `openmoq/moxygen` as needed (see Section 6).

The coverage workflow runs as part of the CI pipeline on one platform (Ubuntu) to avoid redundant instrumented builds across the full matrix:

```yaml
- name: Build with coverage
  run: |
    cmake -S . -B build-cov -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="--coverage" \
      -DCMAKE_PREFIX_PATH="$(cat .scratch/cmake_prefix_path.txt)"
    cmake --build build-cov

- name: Run tests for coverage
  run: ctest --test-dir build-cov --output-on-failure

- name: Collect and report coverage
  run: |
    lcov --capture --directory build-cov --output-file coverage.info
    lcov --remove coverage.info '/usr/*' '*/deps/*' '*/tests/*' --output-file coverage.info
    genhtml coverage.info --output-directory coverage-html

- name: Upload coverage report
  uses: actions/upload-artifact@v4
  with:
    name: coverage-report
    path: coverage-html/
```

> **⚠️ Remaining Open Items:**
>
> - **Coverage targets:** Define minimum coverage thresholds (e.g., 70% line coverage) and whether coverage regressions are blocking or advisory.

### 4.4 Test Failure Notifications

All test and build failures across both the moxygen sync pipeline and the o-rly relay CI are reported to:

- **Email:** github-notifications@openmoq.org
- **Slack:** `#github-notifications`

Notifications include a pass/fail summary and a direct link to the failing workflow run.

---

## 5. Moxygen Fork CI — Artifact Publishing Strategy

### 5.1 Motivation

The o-rly relay builds depend on the entire Meta C++ dependency stack (folly, fizz, wangle, mvfst, proxygen) plus moxygen itself. Building these from source on every o-rly CI run takes 30–60+ minutes even with caching. To enable fast and accurate builds of all o-rly targets, the `openmoq/moxygen` fork CI should produce pre-built artifact bundles that o-rly CI can download directly.

### 5.2 Required Artifacts

The moxygen fork CI should build and publish **per-platform static library bundles** on every merge to `main`. Each bundle contains the installed output of moxygen and all its transitive Meta dependencies (static libraries, headers, CMake config files):

| Artifact Name | Build Environment | Architecture | Consumer |
|---|---|---|---|
| `moxygen-ubuntu-22.04-x86_64.tar.gz` | Ubuntu 22.04 runner | x86_64 | o-rly native Ubuntu CI build |
| `moxygen-macos-14-arm64.tar.gz` | macOS 14+ runner | ARM64 | o-rly native macOS CI build |
| `moxygen-bookworm-amd64.tar.gz` | `debian:bookworm` container | x86_64 | Docker AMD64 image (`Dockerfile.ci`) |
| `moxygen-bookworm-arm64.tar.gz` | `debian:bookworm` container on ARM64 runner | ARM64 | Docker ARM64 image (`Dockerfile.ci`) |

**glibc compatibility:** Docker-targeted artifacts (`bookworm-*`) must be built on Debian bookworm (glibc 2.36) to guarantee binary compatibility with the `debian:bookworm-slim` runtime base. Artifacts built on a newer glibc (e.g., Ubuntu 24.04 / glibc 2.39) will fail at runtime in the container due to glibc symbol versioning. Native CI artifacts (Ubuntu, macOS) only run on matching host environments and are not subject to this constraint.

Artifacts are keyed by the moxygen commit SHA and published as **GitHub Release assets** (tagged `build-<sha>`) or **GitHub Actions artifacts** attached to the CI run. The release approach is preferred because artifacts are addressable by tag, persist indefinitely, and can be downloaded across repositories without authentication for public repos.

### 5.3 Integration with o-rly CI

When moxygen artifacts are available, the o-rly CI "Build dependencies" step changes from building from source:

```yaml
# Current approach (builds from source, ~30-60 min on cache miss)
- name: Build dependencies
  if: steps.deps-cache.outputs.cache-hit != 'true'
  run: ./scripts/setup-deps.sh
```

to downloading pre-built artifacts:

```yaml
# Future approach (download artifacts, ~30 seconds)
- name: Download moxygen artifacts
  run: |
    MOXYGEN_SHA=$(git -C deps/moxygen rev-parse HEAD)
    gh release download "build-${MOXYGEN_SHA}" \
      --repo openmoq/moxygen \
      --pattern "moxygen-${{ matrix.artifact-suffix }}.tar.gz" \
      --dir .scratch/
    tar xzf .scratch/moxygen-*.tar.gz -C .scratch/
```

The rest of the o-rly pipeline (configure, build, test) remains unchanged — `CMAKE_PREFIX_PATH` still points to `.scratch/`.

### 5.4 Docker Path Transition

With per-architecture artifacts (including `linux-arm64`), the Docker build can switch from the full multi-stage `docker/Dockerfile` to the simplified `docker/Dockerfile.ci`. The CI would:

1. Download the moxygen artifact bundle for each target architecture.
2. Build o-rly natively per-arch using a CI matrix (or cross-compilation).
3. Package each binary into the minimal `Dockerfile.ci` runtime image.
4. Create a multi-arch manifest using `docker manifest create`.

This eliminates QEMU emulation and the heavy in-Docker build, reducing Docker CI time from 1–2 hours to minutes.

### 5.5 Implementation Status

The moxygen fork CI artifact publishing is **deferred to a separate task**. Until it is implemented, o-rly CI builds moxygen from the submodule source using `scripts/setup-deps.sh` with aggressive caching keyed by the submodule commit SHA. The o-rly CI workflows are designed so the dependency acquisition step is isolated and can be swapped without changing any other part of the pipeline.

---

## 6. Open Items Summary

| # | Item | Priority | Section | Status |
|---|---|---|---|---|
| 1 | Add Rocky Linux 9 to CI build matrix | Medium | 3.1 | TBD |
| 2 | ~~Confirm container registry~~ | — | 3.2 | ✅ GHCR confirmed |
| 3 | ~~Confirm JUnit XML reporting and select Action~~ | — | 4.2 | ✅ JUnit XML via CTest + mikepenz/action-junit-report |
| 4 | ~~Codecov integration~~ | — | 4.3 | ✅ Replaced with self-hosted lcov/genhtml + Actions artifacts |
| 5 | ~~Define Docker build strategy~~ | — | 3.1.1 | ✅ Dual Dockerfile approach (full + CI) |
| 6 | Document build toolchain requirements (compiler, CMake, etc.) | Medium | 3.1 | TBD |
| 7 | ~~Define Dockerfile location and multi-stage build structure~~ | — | 3.1.1 | ✅ `docker/Dockerfile` (full) + `docker/Dockerfile.ci` (simplified) |
| 8 | Establish benchmark tooling and regression policy | Low | 4.1 | TBD |
| 9 | Define flaky test handling policy | Low | 4.1 | TBD |
| 10 | ~~Implement automated submodule update PR workflow~~ | — | 2.2 | ✅ `.github/workflows/submodule-update.yml` |
| 11 | Implement moxygen fork CI artifact publishing | High | 5 | TBD (deferred) |
| 12 | Define coverage thresholds and regression policy | Medium | 4.3 | TBD |
