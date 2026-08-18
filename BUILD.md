# Building moqx

A standard CMake preset build. moqx links
[moxygen](https://github.com/openmoq/moxygen) as an installed package; everything
else is fetched at configure time by
[CPM](https://github.com/cpm-cmake/CPM.cmake). Pinned revisions live in
[/cmake/dependencies.cmake](/cmake/dependencies.cmake).

## Prerequisites

- **CMake 3.23+**, **Ninja**, a **C++20 compiler** (GCC 11+ / Clang 14+).
  [`scripts/install-system-deps.sh`](/scripts/install-system-deps.sh) fetches a
  current CMake from PyPI when the distro's is older (e.g. Ubuntu 22.04 ships
  3.22, but reflect-cpp needs 3.23).
- **System libraries** — [`scripts/install-system-deps.sh`](/scripts/install-system-deps.sh)
  (apt/dnf/brew), or install by hand: OpenSSL, boost, glog, gflags,
  double-conversion, libevent, sodium, zstd, fmt, c-ares, libunwind, zlib, brotli,
  gperf. Both modes need them: folly & co. resolve them from the system.
- **ccache** is used automatically when it's on `PATH`.

## How dependencies work

moqx doesn't compile moxygen in-tree; it `find_package`es an **installed** moxygen
and builds against it — into `build/<profile>` (`build/default`, `build/san`, …).
The only choice is where that install comes from:

- **Prebuilt with fallback** — the prebuilt when one is published, else the source
  build. The default choice, and what CI uses: moxygen decides what it publishes,
  so this is the only setting that always yields a build.
- **Prebuilt** — download the published tarball for the pinned `MOXYGEN_REV` and
  your platform. Fast, and errors rather than compiling when there is none — take
  it when a slow build is worse for you than a failure.
- **From source** — compile moxygen into an install prefix, then build moqx
  against it. Works for any revision or platform, and lets you develop moxygen and
  moqx together.

The moqx build is identical whichever it is; only the prefix differs.

moxygen is itself a superbuild: its `standalone/` tree compiles Meta's
folly/fizz/wangle/mvfst/proxygen plus the
[openmoq/picoquic](https://github.com/openmoq/picoquic) fork. Those revisions are
pinned inside moxygen, in its
[`build/deps/github_hashes/`](https://github.com/openmoq/moxygen/tree/main/build/deps/github_hashes),
not here. moqx's [/cmake/dependencies.cmake](/cmake/dependencies.cmake) pins the CPM
ones: moxygen, catapult, reflect-cpp, yaml-cpp.

The resolved dependencies live in a cache outside the repo. After a configure,
`deps/<name>` links to each dependency's source and `build/<profile>/moxygen` to
the moxygen install prefix — see [/deps/README.md](/deps/README.md).

## Build

Three scripts, each taking the profile (`default` | `san` | `tsan`, default
`default`) as its first argument, each shown below beside its raw cmake:

- [`scripts/configure.sh`](/scripts/configure.sh) — picks where moxygen comes from, configures
  `build/<profile>` from scratch. Run once per profile; that dir's CMake cache is
  the only state.
- [`scripts/build.sh`](/scripts/build.sh) — compiles.
- [`scripts/test.sh`](/scripts/test.sh) — runs the suite.

`configure.sh` and `build.sh` take `-j N` for compile parallelism, or `MOQX_BUILD_JOBS`
in the environment. Either overrides the default in both directions: go well above the
core count to farm out to distcc, below it on a host short on RAM.

**Prebuilt with fallback** — the default:

```bash
scripts/configure.sh --moxygen prebuilt-with-fallback
scripts/build.sh                                      # compile moqx
scripts/test.sh
```
Attempts the prebuilt and builds from source if that fails for any reason,
transient ones included.

Falling back on everything is deliberate: a slow build beats a failed one, and
re-running costs less than a guess about which failures are permanent.

No raw-cmake equivalent: the two paths are separate CMake projects, and choosing
between them is what the wrapper is for.

`MOQX_MOXYGEN_FALLBACK=off` reduces it to `--moxygen prebuilt`, for when a bad pin
would otherwise have every CI lane compiling folly.

**Prebuilt:**

```bash
scripts/configure.sh --moxygen prebuilt   # download prebuilt moxygen, configure
scripts/build.sh                          # compile moqx
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
scripts/configure.sh --moxygen from-source   # build moxygen -> a prefix, configure moqx against it
scripts/build.sh                             # compile moqx
scripts/test.sh
```
Raw equivalent — the [superbuild](/superbuild) builds the moxygen prefix, then the
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
scripts/configure.sh --moxygen from-source --moxygen-dir ~/src/moxygen   # once
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
scripts/configure.sh san --moxygen from-source    # or tsan
scripts/build.sh san
scripts/test.sh san
```

Sanitizers must instrument the dependencies too, so `san --moxygen from-source`
builds an instrumented moxygen as well; moqx lands in `build/san` (or
`build/tsan`).

The moqx configure measures the prefix's actual instrumentation (an `nm` probe
for `__asan_init`/`__tsan_init` in an installed moxygen archive) and refuses a
mismatch — for raw `cmake` invocations, downloaded prebuilts, and hand-built
prefixes alike.

`prebuilt` and `prebuilt-with-fallback` are refused for these profiles, since
neither can produce an instrumented moxygen. `MOQX_ALLOW_UNINSTRUMENTED_DEPS=1`
overrides that to sanitize moqx's own TUs only — what the per-PR asan lane does.
Its fallback builds the uninstrumented stack too, so a missing prebuilt cannot
quietly promote that lane to a full instrumented build.

Instrumented TUs peak over 2 GB each, enough for the core count to OOM the compiler
on a smaller host, so these profiles derate the default job count by free RAM. `-j` and
`MOQX_BUILD_JOBS` still win outright.

## Custom presets

Profiles are CMake presets. Add your own in `CMakeUserPresets.json`
(gitignored), inherit `default` (that keeps `binaryDir` at `build/<name>`,
which the scripts rely on), and the three scripts accept its name:
`scripts/configure.sh my-preset --moxygen prebuilt-with-fallback && scripts/build.sh my-preset`.
A preset that enables `MOQX_ENABLE_SANITIZERS` or `MOQX_ENABLE_TSAN` gets a
matching instrumented moxygen from `--moxygen from-source` — derived from the
preset's own cache variables, overridable with the `MOQX_MOXYGEN_PROFILE` env
var. Other presets build/link the default-flag moxygen, and `MOQX_MOXYGEN_PROFILE`
naming an instrumented one there is refused: moqx would carry no sanitizer flags
of its own, leaving the interceptors undefined at link.

## Docker, formatting, IDE, CI

- **Docker** — [`docker/Dockerfile`](/docker/Dockerfile) builds via the same
  `cmake --preset` flow, with targets `relay` (default) and `interop-client`. Its
  `moxygen` stage resolves the prefix the same way `prebuilt-with-fallback` does,
  and is keyed on the pin rather than on `src/` so a source change reuses it.
- **Format / lint** (CI requires clang-format-19) —
  [`scripts/dev/format.sh`](/scripts/dev/format.sh) `[--check]`,
  [`scripts/dev/lint.sh`](/scripts/dev/lint.sh) `build/default`.
- **CLion** — point its CMake profile at `cmake --preset default`; for from-source,
  run `scripts/configure.sh --moxygen from-source` first, then add
  `-DMOQX_MOXYGEN_PREBUILT=OFF -DCMAKE_PREFIX_PATH=<prefix>` to the profile.
- **CI / automation** — [/docs/ci-architecture.md](/docs/ci-architecture.md).
