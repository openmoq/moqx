# Building moqx

## Supported Platforms

| Platform | Status | Notes |
|----------|--------|-------|
| Ubuntu 22.04 (Jammy) | Tested in CI | Primary dev/CI platform |
| Debian 12 (Bookworm) | Tested (Docker build) | Docker image base |
| Fedora / RHEL | TBD | `install-system-deps.sh` has dnf support; not yet CI-tested |
| macOS (Homebrew) | TBD | `install-system-deps.sh` has brew support; not yet CI-tested |

## Prerequisites

- CMake 3.25+
- Ninja, C++20 compiler (GCC 11+ / Clang 14+)
- `gh` CLI (authenticated, for downloading release artifacts)
- System libraries -- `build.sh setup` checks and reports what's missing

### Installing CMake 3.25+ (Ubuntu/Debian)

Ubuntu 22.04 ships cmake 3.22 which is too old. Install from the Kitware APT repo:

```bash
# Remove distro cmake if installed
sudo apt-get remove --purge cmake

# Add Kitware signing key and repo
sudo apt-get install -y gpg wget
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null \
  | gpg --dearmor - | sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null
echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main" \
  | sudo tee /etc/apt/sources.list.d/kitware.list

sudo apt-get update
sudo apt-get install -y cmake
cmake --version   # should show 3.25+
```

Ubuntu 24.04+ ships cmake 3.28 -- no extra steps needed.

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
apt-get update && apt-get install -y gpg wget lsb-release sudo git gh
# Install cmake 3.25+ from Kitware (see above)
# Then:
git submodule update --init
sudo deps/moxygen/standalone/install-system-deps.sh
./scripts/build.sh setup --from-source   # no gh auth = build from source
./scripts/build.sh
./scripts/build.sh test
```

## Quick Start

```bash
git clone https://github.com/openmoq/moqx.git && cd moqx
git submodule update --init
sudo deps/moxygen/standalone/install-system-deps.sh

./scripts/build.sh setup      # download prebuilt moxygen deps (~1 min)
./scripts/build.sh            # configure + build
./scripts/build.sh test       # run tests (77 tests, <1s)
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
