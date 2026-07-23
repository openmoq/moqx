# Building moqx

The Quick Start is in the top-level [README](README.md#quick-start). This
document is the detailed build reference: the procedure annotated with
what each step does, plus prereqs, dependency modes, profiles, Docker,
formatting, and CI.

## Supported Platforms

| Platform | Status | Notes |
|----------|--------|-------|
| Ubuntu 22.04 (Jammy) | Tested in CI | Primary dev/CI platform |
| Debian 12 (Bookworm) | Tested (Docker build) | Docker image base |
| Ubuntu/Debian derivatives (Mint, Pop!_OS, ...) | Supported | Detected via `ID_LIKE`, mapped to the Ubuntu/Debian base artifacts; override with `MOQX_PLATFORM` if detection is wrong |
| Fedora / RHEL | TBD | `install-system-deps.sh` has dnf support; not yet CI-tested |
| macOS (Homebrew) | TBD | `install-system-deps.sh` has brew support; not yet CI-tested |

## Build Procedure

Six steps. Each links to its detail section.

1. **Clone and init the moxygen submodule.**
   `git clone … && cd moqx && git submodule update --init --recursive` —
   the submodule pins the exact moxygen commit the build will use
   (see [Dependency Modes](#dependency-modes)).
2. **Ensure CMake 3.22+** is on `PATH`. All current targets ship a
   new-enough version: Ubuntu 22.04+, Debian 12+, recent macOS Homebrew.
   `build.sh` aborts early if cmake is missing or too old.
3. **Install system libraries.** Required in *both* dependency modes —
   see the system-libraries bullet in [Prerequisites](#prerequisites)
   for why. One-liner:
   `sudo deps/moxygen/standalone/install-system-deps.sh`.
4. **Stage moxygen.** `./scripts/build.sh setup` — defaults to
   `--from-release` (downloads the prebuilt tarball, ~1 min); falls back
   to source build if unavailable. See [Dependency Modes](#dependency-modes)
   for `--from-source`, `--moxygen-dir`, SHA pinning, etc.
5. **Build moqx.** `./scripts/build.sh` — configures and builds.
   See [Build Profiles](#build-profiles) for `default` vs `san`.
6. **Run tests.** `./scripts/build.sh test` — 77 tests, sub-second.

If you'd rather build in a clean container, skip to
[Building from a Fresh Ubuntu Docker](#building-from-a-fresh-ubuntu-docker-reproducible-build).

## Prerequisites

- **CMake 3.22+** — required by moqx top-level `CMakeLists.txt`. All
  current targets ship a new-enough version out of the box: Ubuntu
  22.04+, Debian 12+, recent macOS Homebrew. `build.sh` enforces this
  and aborts early if cmake is missing or too old (override with
  `MOQX_SKIP_CMAKE_CHECK=1`).
- Ninja, C++20 compiler (GCC 11+ / Clang 14+).
- `curl` for downloading the moxygen release tarball
  ([`setup-deps-tarball.sh`](scripts/setup-deps-tarball.sh)). No `gh` CLI is
  required. Set `GITHUB_TOKEN`/`GH_TOKEN` only if you hit api.github.com's
  unauthenticated rate limit.
- **System libraries** — required in **both** dependency modes:
  libssl, libfmt, libglog, libgflags, libdouble-conversion, libevent,
  libsodium, libzstd, libboost, c-ares, libunwind, zlib, gperf. The
  release tarball ships folly/fizz/wangle/mvfst/proxygen statically, but
  moxygen's CMake config does `find_dependency(fmt, Glog, ...)` and folly
  itself transitively wants OpenSSL/Boost. Install all with:
  ```bash
  sudo deps/moxygen/standalone/install-system-deps.sh
  ```
  (`build.sh setup`'s system-dep check only fires when falling back to a
  source build — it does not preempt the build-step failure if libs are
  missing.)

### Installing CMake

moqx requires CMake 3.22+. All current targets ship a new-enough version
out of the box:

**Ubuntu 22.04+ / Debian 12+:**

```bash
sudo apt-get install -y cmake
cmake --version   # should show 3.22+
```

**macOS (Homebrew):**

```bash
brew install cmake     # or `brew upgrade cmake` if already present
cmake --version
```

### Installing System Dependencies

```bash
sudo deps/moxygen/standalone/install-system-deps.sh
```

### Building from a Fresh Ubuntu Docker (Reproducible Build)

This is handy for verifying the build on a clean system or for contributors
who don't want to install deps on their host:

```bash
docker run --rm -it -v "$PWD":/src -w /src ubuntu:22.04 bash

# Inside the container:
apt-get update && apt-get install -y cmake ninja-build sudo git curl ca-certificates
git submodule update --init --recursive
sudo deps/moxygen/standalone/install-system-deps.sh
./scripts/build.sh setup --from-source   # build from source (no release artifacts available offline)
./scripts/build.sh
./scripts/build.sh test
```

## Dependency Modes

moqx depends on moxygen (and its Meta deps: folly, fizz, wangle, mvfst, proxygen).
The `deps/moxygen` submodule pins the exact version. Two ways to get these deps:

| Mode | Command | Time | When to use |
|------|---------|------|-------------|
| **from-release** | `build.sh setup` | ~1 min | Default -- downloads CI-built artifacts |
| **from-source** | `build.sh setup --from-source` | 15-30 min | Full control, or when artifacts unavailable |

Both accept an optional commit SHA to override the submodule pointer:

```bash
build.sh setup --from-release abc1234   # artifacts for specific moxygen commit
build.sh setup --from-source abc1234    # build specific commit from source
```

To build against a local moxygen checkout (for iterating on moxygen itself):

```bash
build.sh setup --from-source --moxygen-dir ~/src/moxygen
```

Default (no SHA or dir) uses the current submodule HEAD.
Falls back from release to source if artifacts aren't available.
Use `--no-fallback` to fail instead. Use `--clean` to wipe `.scratch/` first.

## Build Profiles

```
build.sh setup [--from-release [SHA]|--from-source [SHA]] [--moxygen-dir DIR] [--no-fallback] [--clean]
build.sh [--profile default|san] [--build-dir DIR]
build.sh test [--build-dir DIR] [-- CTEST_ARGS...]
```

| Profile | Build dir | Description |
|---------|-----------|-------------|
| `default` | `build/` | RelWithDebInfo |
| `san` | `build-san/` | Debug + ASAN/UBSAN |

## Formatting and Linting

CI requires clang-format-19. Check before pushing:

```bash
./scripts/format.sh --check    # verify (dry-run)
./scripts/format.sh            # fix in-place
./scripts/lint.sh build        # clang-tidy (requires prior build)
```

## PR Process

1. Create a branch, push changes
2. CI runs: format check + build/test (default + ASAN)
3. All checks must pass before merge
4. Squash-and-merge preferred for single-feature PRs

## CI and Automation

See [design/ci-architecture.md](design/ci-architecture.md) for the full CI pipeline:
upstream sync, submodule updates, build/publish/release, and auto-deploy.

## Developer IDEs

### CLion

CLion can build moqx directly via its CMake integration. You still need the
same prerequisites as the command-line build (CMake 3.22+, system libraries,
and a staged moxygen install). The steps below wire CLion to the local
dependency tree under `.scratch/`.

#### 1. Stage dependencies (first time, or after dep changes)

Before CLion can configure the project, moxygen must be present at
`.scratch/moxygen-install`. Run:

```bash
./scripts/build.sh setup --from-source
```

This installs moxygen and its Meta dependencies into `.scratch/moxygen-install`.
It takes 15–30 minutes on a first run.

You only need to re-run this when **dependencies change** — for example, after
updating the `deps/moxygen` submodule or when `.scratch/` has been cleaned.
Day-to-day moqx source edits do not require re-running setup.

#### 2. Configure CMake in CLion

Open the moqx project root in CLion, then go to
**Settings → Build, Execution, Deployment → CMake** and edit the **Debug**
profile. Add these CMake options:

```
-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_PREFIX_PATH=$CMakeProjectDir$/.scratch/moxygen-install -DGFLAGS_SHARED=ON
```

`$CMakeProjectDir$` is a CLion macro that expands to the project root, so
CMake can find the staged moxygen package config.

If you configure before running setup, CMake will fail because
`.scratch/moxygen-install` does not exist yet — that is expected.

#### 3. Reload and build

After setup completes, reload the CMake project in CLion
(**Tools → CMake → Reload CMake Project**, or the reload button in the CMake
tool window). Configuration should succeed and you can build and run targets
from the IDE as usual.
