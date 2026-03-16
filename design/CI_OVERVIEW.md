# CI Overview: moxygen + o-rly

## Cross-repo dependency

```
┌─────────────────────────────────────────────────────────────┐
│  moxygen (library)                                          │
│                                                             │
│  ci-main ──► snapshot-latest release (8 tarballs, ~5.4GB)   │
│                         │                                   │
└─────────────────────────┼───────────────────────────────────┘
                          │  submodule SHA pins which tarball
                          ▼
┌─────────────────────────────────────────────────────────────┐
│  o-rly (application)                                        │
│                                                             │
│  ci-main ──► Docker image (ghcr.io/openmoq/o-rly)          │
│              + binary tarball (~32MB)                        │
└─────────────────────────────────────────────────────────────┘
```

o-rly's `deps/moxygen` submodule pins a moxygen commit. `setup-deps-tarball.sh`
downloads the matching pre-built tarball from moxygen's release, so o-rly never
builds moxygen from source in CI.

---

## moxygen — 5 workflows

### 1. `ci pr` — Pull request verification

**Trigger:** PR to main | **Wall clock:** ~15 min

| Job | Runner | Time | Purpose |
|-----|--------|------|---------|
| linux | ubuntu-22.04 | 1-16min | Build + test (ccache hit = fast) |
| macos | macos-15 | 2min | Build + test |
| pico (linux) | ubuntu-22.04 | 2min | Minimal transport-only build |
| pico (macos) | macos-15 | 2min | Minimal transport-only build |
| asan debug | ubuntu-22.04 | 15-26min | ASAN/UBSAN build + test |

All 5 run in parallel. Required checks: `linux`, `macos`.

### 2. `ci main` — Build, Publish, Release, Notify

**Trigger:** push to main | **Wall clock:** ~72 min

```
build (5 jobs, parallel, ~26min)
  └─► publish (4 jobs, parallel, ~42min)
        └─► release (1 job, ~3min)
              └─► notify (Slack + email)
```

| Publish platform | Runner | Time | Tarball size |
|-----------------|--------|------|-------------|
| ubuntu-22.04-amd64 | ubuntu-22.04 | 33min | 980 MB |
| bookworm-amd64 | ubuntu-22.04 (container) | 42min | 951 MB |
| bookworm-arm64 | ubuntu-22.04-arm (container) | 28min | 936 MB |
| macos-15-arm64 | macos-15 | 16min | 55 MB |

Each platform also produces a `-dbg.tar.gz` (split debug symbols, 450-699 MB).
Release creates/updates `snapshot-latest` pre-release with all 8 tarballs.

### 3. `upstream sync` — Daily upstream mirror

**Trigger:** daily 07:00 UTC + manual | **Time:** <1 min

- Finds latest green commit on `facebookexperimental/moxygen`
- Advances `origin/upstream` branch
- Creates `sync/<sha>` PR if upstream is ahead of main
- Notifies Slack/email on conflicts or failures

### 4. `sync auto-merge` — Merge green sync PRs

**Trigger:** `ci pr` completes on `sync/*` branch | **Time:** <1 min

- Merges the sync PR if CI passed

### 5. `version release` — Manual tagged release

**Trigger:** manual (version input) | **Time:** <1 min

- Promotes existing `snapshot-latest` artifacts to a versioned `vX.Y.Z` release
  (no rebuild)

---

## o-rly — 3 workflows

### 1. `ci pr` — Pull request verification

**Trigger:** PR to main | **Wall clock:** ~5 min

| Job | Runner | Time | Purpose |
|-----|--------|------|---------|
| check-format | ubuntu-latest (trixie) | <1min | clang-format check |
| linux | ubuntu-22.04 | 4min | Build + test against moxygen tarball |
| asan debug | ubuntu-22.04 | 4min | ASAN/UBSAN build + test |

### 2. `ci main` — Build, Publish (Docker), Release, Notify

**Trigger:** push to main | **Wall clock:** ~10 min

```
check-format (<1min)
build (2 jobs, parallel, ~5min)
  └─► publish (~5min): Docker build + smoke test + push to GHCR
        └─► release: snapshot-latest with binary tarball (~32MB)
              └─► notify (Slack + email)
```

Publish builds a multi-stage Docker image (bookworm), runs `ldd` + `/info`
endpoint smoke test, pushes to `ghcr.io/openmoq/o-rly:{sha}` + `:latest`.

### 3. `update moxygen submodule` — Manual submodule bump

**Trigger:** manual | **Time:** <1 min

- Advances `deps/moxygen` to latest moxygen main, creates a PR

---

## Typical flow: upstream change to deployed Docker image

```
facebookexperimental/moxygen  (upstream commit lands)
        │
        ▼  daily 07:00 UTC
   upstream sync creates sync/<sha> PR on openmoq/moxygen
        │
        ▼  ci pr runs (~15 min)
   sync auto-merge merges PR
        │
        ▼  push to main triggers ci main (~72 min)
   snapshot-latest updated with new tarballs
        │
        ▼  manual: update moxygen submodule on o-rly
   PR created, ci pr runs (~5 min), merge
        │
        ▼  push to main triggers ci main (~10 min)
   Docker image pushed to GHCR, binary tarball released
```

---

## Linkage profile

The build uses a hybrid static/dynamic strategy. Meta's libraries (the hard-to-package
ones) are statically linked into the binary. Common system libraries stay dynamic so
the binary tracks the host distro's versions naturally.

### Moxygen tarball (all platforms: ubuntu-22.04, bookworm amd64/arm64, macos)

The linkage strategy is defined in `standalone/CMakeLists.txt` and is the same across
all 4 platform targets. Each tarball is a flat cmake prefix (`lib/*.a` + `lib/cmake/*/`
+ `include/*/`). Everything below is static `.a` — consumers link it in at build time:

| Component | Linkage | Notes |
|-----------|---------|-------|
| folly, fizz, wangle, mvfst, proxygen | Static .a | FetchContent, never installed as .so |
| fmt, sodium, zlib | Static .a | Bundled to avoid version mismatches |
| boost | Static .a | `Boost_USE_STATIC_LIBS=ON` |

The tarball's cmake configs (e.g. `folly-targets.cmake`) reference these system
libraries by `.so` path. Consumers inherit the dynamic linkage at configure time:

| Component | Why dynamic |
|-----------|-------------|
| glog, gflags, double-conversion | `.so` preference avoids dual-linkage under ASAN |
| OpenSSL (libssl, libcrypto) | Distro-managed for security patches |
| libevent, zstd, lz4, snappy | Common system libs, stable ABI |
| libunwind | Static `.a` is non-PIC on Ubuntu, won't link |

### o-rly binary (ubuntu-22.04, CI + tarball artifact)

Single static executable with all Meta deps baked in. At runtime it needs:

```
libssl3  libcrypto3  libgoogle-glog0v5  libgflags2.2
libdouble-conversion3  libevent-2.1-7  libzstd1  libunwind8
libc  libstdc++  libpthread
```

These are all standard Ubuntu 22.04 system packages — no custom `.so` files needed.

### o-rly Docker image (bookworm, GHCR)

Multi-stage build: build stage has `-dev` packages, runtime stage has only `.so` packages.
Same binary linkage as above, different distro package names:

```dockerfile
# Runtime .so packages (debian:bookworm-slim base provides libc, libstdc++, libssl3)
libunwind8  libsodium23  libboost-context1.74.0
libgoogle-glog0v6  libgflags2.2  libdouble-conversion3
```

### Summary

```
o-rly binary
  ├── STATIC: folly fizz wangle mvfst proxygen boost fmt sodium zlib
  └── DYNAMIC: openssl glog gflags double-conversion libevent zstd libunwind libc libstdc++
```

The static half is ~30 MB in the binary. The dynamic half is ~5 MB of system `.so` files
that every Linux box already has (or installs via a single `apt install` line).

---

## Cost summary (per event, GitHub-hosted minutes)

| Event | Billable minutes (approx) |
|-------|--------------------------|
| moxygen PR | ~25 min (linux + asan; macos billed 10x = ~40 min effective) |
| moxygen main push | ~130 min (build + publish across 4 platforms + macos multiplier) |
| o-rly PR | ~10 min |
| o-rly main push | ~15 min |
