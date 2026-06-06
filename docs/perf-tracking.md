# Performance Regression Tracking

This document describes the CI-integrated performance testing and regression
tracking system for moqx.

## Overview

Every push to `main` (and every PR) triggers an end-to-end relay performance
test on dedicated hardware. Results are tracked over time and surfaced via:

- **PR comments** — comparison table showing current vs baseline with regression flags
- **GitHub Pages dashboard** — time-series charts for all tracked metrics
- **CI step summaries** — inline results in the Actions run view

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

Regression detection uses a rolling 10-run average on `main` as baseline.
Warnings are non-blocking (PRs are not failed).

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
| `PERF_SSH_KEY` | Contents of `perf-key` (private key) |
| `PERF_RELAY_HOST` | `root@<relay-ip>` |
| `PERF_CLIENT_HOST` | `root@<client-ip>` |

### 5. Create gh-pages Branch

```bash
git checkout --orphan gh-pages
git rm -rf .
cp -r gh-pages/* .
git add index.html data/
git commit -m "Initial performance dashboard"
git push origin gh-pages
```

### 6. Enable GitHub Pages

Repository → Settings → Pages → Source: Deploy from branch → `gh-pages` / root.

## Files

| File | Purpose |
|------|---------|
| `scripts/perf-test-ci.sh` | CI orchestration (deploy, run, collect) |
| `scripts/perf-results-to-json.sh` | Parse client output → JSON |
| `scripts/perf-compare.py` | Regression detection + markdown |
| `scripts/perf-test.sh` | Underlying test runner (unchanged) |
| `scripts/perf-metrics.sh` | Prometheus metrics poller (unchanged) |
| `.github/workflows/ci-main.yml` | perf-test job (commits results) |
| `.github/workflows/ci-pr.yml` | perf-test job (posts PR comment) |
| `gh-pages/index.html` | Dashboard (Chart.js) |
| `gh-pages/data/index.json` | Run manifest |

## Viewing Results

- **Dashboard:** `https://<org>.github.io/moqx/` (after GitHub Pages is enabled)
- **PR comments:** Automatically posted/updated on every PR
- **Artifacts:** Available in the Actions run under "perf-results"
- **Step summary:** Visible inline in the GitHub Actions job view

## Concurrency

The `perf-test` job uses a concurrency group (`perf-test`) with
`cancel-in-progress: false`. If multiple CI runs trigger simultaneously, they
queue and execute sequentially to avoid conflicting on the shared VMs.

## Data Retention

The gh-pages index keeps the last 180 runs (~6 months at 1 run/day). Older
entries are pruned during the commit step.
