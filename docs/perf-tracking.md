# Performance Regression Tracking

This document describes the standalone performance testing and regression
tracking workflow for moqx.

## Overview

Performance tests run on dedicated hardware via a standalone GitHub workflow.
The canonical trend comes from a nightly run against `main` HEAD; members and
maintainers can also trigger ad-hoc runs against any branch. Results are
surfaced via:

- **GitHub Pages dashboard** — time-series charts for all tracked metrics (nightly trend)
- **CI step summaries** — inline results in the Actions run view
- **PR comments** — comparison table, optionally posted to a PR when a run sets
  the `pr` input (otherwise the same report lives in the step summary)

Primary workflow: [`.github/workflows/perf-test.yml`](/.github/workflows/perf-test.yml)

## Triggering

The perf workflow is intentionally decoupled from per-push/per-PR CI so it never
competes with PR builds on the shared self-hosted VMs and never auto-runs
untrusted PR code.

- **Nightly schedule:** `cron: '0 5 * * *'` (05:00 UTC) against `main` HEAD.
  This is the only trigger that publishes to GitHub Pages.
- **Manual run:** Actions tab → `perf test` (`workflow_dispatch`). Pick the
  branch/tag under test in the native **"Use workflow from"** selector, then
  tune `subscribers`/`duration`, toggle `compare` to render a regression report,
  and set `pr` to also post that report as a PR comment. Manual runs upload
  artifacts but do not publish to the trend.
- **Reusable call:** from another workflow via `workflow_call` (this form takes
  an explicit `ref` input for the commit/branch/tag to test).

Supported `workflow_dispatch` inputs (schedule runs use the defaults below):

| Input | Default | Description |
|---|---:|---|
| `duration` | `120` | Test duration in seconds |
| `subscribers` | `600` | Peak subscribers |
| `quic_stack` | `both` | Run `both`, `mvfst`, or `picoquic` |
| `compare` | `true` | Compare against the published baseline and render a report into the step summary |
| `pr` | _(blank)_ | PR number to also post the report to; blank = report stays in the step summary only |

The nightly run and default manual dispatch test both stacks sequentially on the
same VM pair. They are not a matrix because the relay and metrics poller are
host-global. Results use separate files (`run-<sha>-mvfst.json` and
`run-<sha>-picoquic.json`), and comparisons use only history for the same stack.
A picoquic failure is initially non-fatal so it cannot prevent the mvfst result
from being published. The dashboard defaults to compare mode, overlaying both
stacks at shared commit positions. Single mode shows one stack's history and
defaults to mvfst. The overlay distinguishes the stacks by color and line style
and reports each stack's congestion-control algorithm.

> **Branch under test:** there is no `ref` dispatch input — the run tests
> whatever branch/tag is chosen in "Use workflow from". The `ref` input exists
> only on the `workflow_call` form for programmatic callers.

> **Subscriber note:** `workflow_dispatch` defaults `subscribers` to `100` for
> quick smoke runs, while the nightly trend is captured at `1000`. The
> comparison always scores against the published `main` trend, so for a
> meaningful report dispatch with `subscribers: 1000`.

> **Referencing a run on a PR without posting:** leave `pr` blank. The
> comparison report is always written to the run's **step summary**, whose URL
> can be pasted into a PR review by hand — zero automated PR footprint.

## Architecture

```
┌──────────────────────────┐
│  CI Runner (self-hosted)  │  Orchestrates via SSH
│  GitHub Actions           │
└──────┬───────────┬────────┘
       │ SSH       │ SSH
       ▼           ▼
┌──────────────┐  ┌──────────────┐
│ Relay VM      │  │ Client VM    │
│ Linode Ded-4  │  │ Linode Ded-4 │
│ Runs: moqx    │  │ Runs:        │
│       server  │  │  perf_client │
│       metrics │  │              │
└──────────────┘  └──────────────┘
       Same Linode region (low latency)
```

## Tracked Metrics

| Metric | Unit | Higher=Better | Regression Threshold |
|--------|------|:---:|---|
| Peak Subscribers | count | ✓ | 5% |
| Throughput | Mbps | ✓ | 5% |
| Throughput per Core | Mbps | ✓ | 5% |
| Subscribers per Core | count | ✓ | 5% |
| Delivery Success | % | ✓ | 2% |
| Relay CPU | % | ✗ | 10% |
| Relay RSS | MB | ✗ | 10% |
| RSS per Session | KB | ✗ | 10% |
| Total Resets | count | ✗ | 50% |
| UDP Errors/s | count | ✗ | 50% |

Regression detection uses a rolling 10-run average on `main` for the same QUIC
stack. Missing `schema_version` and `params.quic_stack` in historical results
are treated as version 1 and mvfst respectively. Warnings are non-blocking
(PRs are not failed).

### Stack Comparability

Each stack uses its native congestion-control spelling and algorithm: mvfst
runs use `bbr2`, while picoquic runs use `bbr`. Consequently, differences
between the two trend lines reflect both QUIC implementation and congestion
control; cross-stack throughput comparisons are indicative, not controlled.
Use each stack's own history to evaluate regressions.

## Test Parameters

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Subscribers | 1000 | Enough to stress relay fan-out |
| Ramp rate | 100/s | 10s to reach peak, then 110s steady state |
| Duration | 120s | Stable measurement window |
| IO threads | 4 | Matches VM core count |
| Client threads | 4 | Matches client VM cores |
| Transport | QUIC | Primary protocol |
| Delivery timeout | 500ms | Detects latency regressions |

## Infrastructure Setup (One-Time)

### 1. Provision VMs

Create two Linode Dedicated 4-core (8GB) instances in the same region:

```bash
# Example: us-east, Ubuntu 22.04
linode-cli linodes create --label moqx-perf-relay --type g6-dedicated-4 --region us-east --image linode/ubuntu22.04
linode-cli linodes create --label moqx-perf-client --type g6-dedicated-4 --region us-east --image linode/ubuntu22.04
```

### 2. Configure VMs

On both VMs:

```bash
# System limits
echo '* soft nofile 65536' >> /etc/security/limits.conf
echo '* hard nofile 65536' >> /etc/security/limits.conf

# Install runtime dependencies (match build platform: Ubuntu 22.04)
apt-get update && apt-get install -y \
  libssl3 libgoogle-glog0v5 libgflags2.2 libdouble-conversion3 \
  libevent-2.1-7 libunwind8 libzstd1 curl

# Install jemalloc (optional, for production-like memory behavior)
apt-get install -y libjemalloc2
ln -sf /usr/lib/x86_64-linux-gnu/libjemalloc.so.2 /lib64/libjemalloc.so.2
```

### 3. Configure SSH Access

```bash
# On CI runner (or locally):
ssh-keygen -t ed25519 -f perf-key -N ""

# Copy public key to both VMs:
ssh-copy-id -i perf-key.pub root@<relay-ip>
ssh-copy-id -i perf-key.pub root@<client-ip>
```

### 4. Add GitHub Secrets

In the repository settings → Secrets and variables → Actions:

| Secret | Value |
|--------|-------|
| `OMOQ_APP_ID` | GitHub App id used for checkout/tokened operations |
| `OMOQ_APP_PRIV_KEY` | GitHub App private key |
| `PERF_SSH_KEY` | Contents of `perf-key` (private key) |
| `PERF_RELAY_HOST` | `root@<relay-ip>` |
| `PERF_CLIENT_HOST` | `root@<client-ip>` |

### 5. Enable GitHub Pages (GitHub Actions Source)

Repository → Settings → Pages → Source: **GitHub Actions**.

The workflow stages a Pages artifact (`perf-out/`) and deploys it with
`actions/deploy-pages`.

## Files

| File | Purpose |
|------|---------|
| [`scripts/perf/perf-test-ci.sh`](/scripts/perf/perf-test-ci.sh) | CI orchestration (deploy, run, collect) |
| [`scripts/perf/perf-results-to-json.sh`](/scripts/perf/perf-results-to-json.sh) | Parse client output → JSON |
| [`scripts/perf/perf-compare.py`](/scripts/perf/perf-compare.py) | Regression detection + markdown |
| [`scripts/perf/perf-test.sh`](/scripts/perf/perf-test.sh) | Underlying test runner |
| [`scripts/perf/perf-metrics.sh`](/scripts/perf/perf-metrics.sh) | Prometheus metrics poller |
| [`.github/workflows/perf-test.yml`](/.github/workflows/perf-test.yml) | Standalone perf workflow (run, compare, stage, deploy) |
| [`status/index.html`](/status/index.html) | Dashboard shell (copied to Pages artifact root) |
| `perf-out/perf/index.json` | Generated run manifest in Pages artifact |
| `perf-out/perf/run-*.json` | Generated per-run result files |

## Viewing Results

- **Dashboard:** `https://openmoq.org/moqx/` (nightly trend)
- **Perf data:** `https://openmoq.org/moqx/perf/index.json`
- **Artifacts:** Available in the Actions run under "perf-results" (every run)
- **Step summary:** Visible inline in the GitHub Actions job view
- **PR comments:** Only when a manual/called run sets the `pr` input; otherwise
  the report stays in the step summary

Notes:

- Every run uploads artifacts for inspection.
- Publishing to Pages is gated to the nightly schedule in the `deploy` job.

## Concurrency

The `perf-test` job uses a concurrency group (`perf-test`) with
`cancel-in-progress: false`. If multiple CI runs trigger simultaneously, they
queue and execute sequentially to avoid conflicting on the shared VMs.

## Data Retention

The Pages payload keeps up to 360 run files, enough for roughly 180 nightly
points per stack (about six months). The manifest is rebuilt newest-first and
capped during staging.
