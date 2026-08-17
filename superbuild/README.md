# moxygen superbuild

Builds **moxygen from source** — the whole Meta stack (folly / fizz / wangle /
mvfst / proxygen) plus the [openmoq/picoquic](https://github.com/openmoq/picoquic)
fork — and installs it as a CMake package prefix for the moqx build to consume
via `find_package(moxygen CONFIG)`. It wraps
[moxygen's `standalone/` tree](https://github.com/openmoq/moxygen/tree/main/standalone)
as an `ExternalProject`, at the revision pinned in
[/cmake/dependencies.cmake](/cmake/dependencies.cmake).

moqx itself is **not** built here — the prebuilt and from-source paths build
moqx identically; only the origin of the moxygen prefix differs. See
[/BUILD.md](/BUILD.md).

## Usage

Normally driven by [`scripts/configure.sh --moxygen from-source`](/scripts/configure.sh),
which builds this into `.scratch/moxygen-build[-<profile>]` and configures moqx
against the result. Raw equivalent:

```bash
cmake -S superbuild -B .scratch/moxygen-build -G Ninja
cmake --build .scratch/moxygen-build   # -> .scratch/moxygen-build/moxygen-install
```

| Knob | Effect |
|------|--------|
| `-DCPM_moxygen_SOURCE=/path` | build a local moxygen checkout |
| `-DMOQX_MOXYGEN_BUILD_ALWAYS=ON` | rebuild on every build (local-checkout iteration) |
| `-DMOQX_MOXYGEN_PROFILE=san\|tsan` | build an instrumented moxygen (matches moqx's sanitizer presets) |
| `-DBOOST_USE_STATIC_LIBS=auto\|on\|off` | override the static/shared Boost probe (`auto` = probe, the default) |

Each knob also reads the same-named env var.
