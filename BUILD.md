# Building moqx

A standard CMake preset build. moqx links
[moxygen](https://github.com/openmoq/moxygen) as an installed package; everything
else is fetched at configure time by
[CPM](https://github.com/cpm-cmake/CPM.cmake). Pinned revisions live in
[cmake/dependencies.cmake](cmake/dependencies.cmake).

## Prerequisites

- **CMake 3.23+**, **Ninja**, a **C++20 compiler** (GCC 11+ / Clang 14+).
  `install-system-deps.sh` fetches a current CMake from PyPI when the distro's is
  older (e.g. Ubuntu 22.04 ships 3.22, but reflect-cpp needs 3.23).
- **System libraries** — `scripts/install-system-deps.sh` (apt/dnf/brew), or
  install by hand: OpenSSL, boost, glog, gflags, double-conversion, libevent,
  sodium, zstd, fmt, c-ares, libunwind, zlib, brotli, gperf. Needed in both modes —
  folly & co. resolve them from the system.
- **ccache** is used automatically when it's on `PATH`.

## How dependencies work

moqx doesn't compile moxygen in-tree; it `find_package`es an **installed** moxygen
and builds against it — into `build/<profile>` (`build/default`, `build/san`, …).
The only choice is where that install comes from — two equal alternatives:

- **Prebuilt** — download the published tarball for the pinned `MOXYGEN_REV` and
  your platform. Fast; needs a matching published platform.
- **From source** — compile moxygen into an install prefix, then build moqx
  against it. Works for any revision or platform, and lets you develop moxygen and
  moqx together.

Either way the moqx build is identical; only the prefix differs.

moxygen is itself a superbuild: its `standalone/` tree compiles Meta's
folly/fizz/wangle/mvfst/proxygen plus the
[openmoq/picoquic](https://github.com/openmoq/picoquic) fork. Those revisions are
pinned inside moxygen, in its
[`build/deps/github_hashes/`](https://github.com/openmoq/moxygen/tree/main/build/deps/github_hashes),
not here. moqx's [cmake/dependencies.cmake](cmake/dependencies.cmake) pins the CPM
ones: moxygen, catapult, reflect-cpp, yaml-cpp.

## Build

The classic trilogy, each taking the profile (`default` | `san` | `tsan`,
default `default`) as its first argument:

- [`scripts/configure.sh`](scripts/configure.sh) — picks the mode, configures
  `build/<profile>` from scratch. Run once per profile; that dir's CMake cache is
  the only state.
- [`scripts/build.sh`](scripts/build.sh) — compiles.
- [`scripts/test.sh`](scripts/test.sh) — runs the suite.

The raw cmake is shown beside each.

**Prebuilt:**

```bash
scripts/configure.sh --mode prebuilt   # download prebuilt moxygen, configure
scripts/build.sh                       # compile moqx
scripts/test.sh
```
Raw equivalent — moqx downloads the prebuilt itself at configure time:
```bash
cmake --preset default && cmake --build build/default && ctest --test-dir build/default --output-on-failure
```
A moxygen install already on `CMAKE_PREFIX_PATH` is used as-is (tried before any
download). No prebuilt for your platform → configure errors and points here; force
a published tag with `-DMOQX_PLATFORM=<tag>`. Platform tags are
`ubuntu-<version_id>-<arch>`, `bookworm-<arch>` (Debian and its derivatives) and
`macos-<major>-<arch>`, `<arch>` ∈ {`amd64`, `arm64`} — what is actually published
is on [openmoq/moxygen's releases](https://github.com/openmoq/moxygen/releases).

**From source:**

```bash
scripts/configure.sh --mode from-source   # build moxygen -> a prefix, configure moqx against it
scripts/build.sh                          # compile moqx
scripts/test.sh
```
Raw equivalent — the [superbuild](superbuild/) builds the moxygen prefix, then the
same moqx build consumes it:
```bash
cmake -S superbuild -B .scratch/moxygen-build -G Ninja   # [-DCPM_moxygen_SOURCE=/path]
cmake --build .scratch/moxygen-build                     # -> .scratch/moxygen-build/moxygen-install
cmake --preset default -DMOQX_MOXYGEN_PREBUILT=OFF \
      -DCMAKE_PREFIX_PATH=$PWD/.scratch/moxygen-build/moxygen-install
cmake --build build/default
```

## Developing moxygen + moqx together

Point `configure.sh` at a local moxygen checkout **once**, then iterate with
two builds:

```bash
scripts/configure.sh --mode from-source --moxygen-dir ~/src/moxygen   # once
# edit ~/src/moxygen/… then:
cmake --build .scratch/moxygen-build    # recompile + reinstall moxygen (incremental)
scripts/build.sh                        # relink moqx against the refreshed install
```

Only your changed moxygen files recompile, and moqx relinks against the refreshed
prefix. `--moxygen-dir` is the only flag you need: `configure.sh` passes both the
prefix and the matching find-modules to the moqx configure. By hand that is two
flags — `-DCMAKE_PREFIX_PATH` for the libraries **and**
`-DCPM_moxygen_SOURCE=/path` for moxygen's MODULE-mode find-modules.

Develop a local catapult instead with `-DCPM_catapult_SOURCE=/path` on the moqx
build — it compiles in-tree, no prefix needed.

## Sanitizers

```bash
scripts/configure.sh san --mode from-source    # or tsan
scripts/build.sh san
scripts/test.sh san
```

Sanitizers must instrument the dependencies too, so `san --mode from-source`
builds an instrumented moxygen as well; moqx lands in `build/san` (or
`build/tsan`).

## Custom presets

Profiles are just CMake presets — add your own in `CMakeUserPresets.json`
(gitignored), inherit `default` (that keeps `binaryDir` at `build/<name>`,
which the scripts rely on), and the trilogy accepts its name:
`scripts/configure.sh my-preset --mode prebuilt && scripts/build.sh my-preset`.
A preset that enables `MOQX_ENABLE_SANITIZERS` or `MOQX_ENABLE_TSAN` gets a
matching instrumented moxygen from `--mode from-source` — derived from the
preset's own cache variables, overridable with the `MOQX_MOXYGEN_PROFILE` env
var; other presets build/link the default-flag moxygen.

## Docker, formatting, IDE, CI

- **Docker** — `docker/Dockerfile` builds via the same `cmake --preset` flow.
- **Format / lint** (CI requires clang-format-19) — `scripts/dev/format.sh [--check]`,
  `scripts/dev/lint.sh build/default`.
- **CLion** — point its CMake profile at `cmake --preset default`; for from-source,
  run `scripts/configure.sh --mode from-source` first, then add
  `-DMOQX_MOXYGEN_PREBUILT=OFF -DCMAKE_PREFIX_PATH=<prefix>` to the profile.
- **CI / automation** — [docs/ci-architecture.md](docs/ci-architecture.md).
