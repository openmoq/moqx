# o-rly
The OpenMOQ Relay

## Prerequisites

- CMake 3.20+
- Ninja
- Python 3
- A C++20 compiler (GCC 11+ or Clang 14+)
- pkg-config, libssl-dev

## Submodules

o-rly depends on [moxygen](https://github.com/openmoq/moxygen), which is
included as a git submodule under `deps/moxygen` (tracking the
`orly-integration` branch). After cloning, initialize it with:

```bash
git submodule update --init
```

## Building Locally

The simplest way to build from scratch is the all-in-one build script:

```bash
scripts/build.sh
```

On the first run this will:

1. Run `scripts/setup-deps.sh` — uses moxygen's `getdeps.py` to fetch
   and build all third-party dependencies (folly, fizz, wangle, mvfst,
   proxygen) into `.scratch/`.
2. Run `scripts/configure.sh` — invokes CMake with the `default` preset,
   pointing `CMAKE_PREFIX_PATH` at the built dependencies.
3. Build o-rly itself with `cmake --build build`.

On subsequent runs, `build.sh` detects whether dependency versions or
the moxygen revision have changed and only rebuilds what is needed.

You can also run the steps manually:

```bash
# 1. Build moxygen and all deps (first time only, or after dep changes)
scripts/setup-deps.sh

# 2. Configure
scripts/configure.sh

# 3. Build
cmake --build build
```

The scratch directory defaults to `.scratch/` in the project root.
Override it with the `ORLY_SCRATCH_PATH` environment variable.

### CMake Presets

| Preset    | Description           |
|-----------|-----------------------|
| `default` | Ninja, RelWithDebInfo |
| `san`     | Debug with ASAN/UBSAN |

## Building with Docker

The Docker build requires no local dependencies beyond Docker itself. It
uses a multi-stage build to cache dependency compilation:

```bash
docker build -f docker/Dockerfile -t o-rly .
```

The stages are:

1. **base** — installs OS-level build tools on Debian 13.
2. **deps** — builds moxygen's third-party dependencies (cached until
   dependency manifests change).
3. **moxygen** — builds moxygen itself (cached until `deps/moxygen`
   changes).
4. **build** — builds o-rly, statically linking against the
   dependencies.
5. **runtime** — minimal Debian slim image containing only the `o-rly`
   binary.

Run the resulting image:

```bash
docker run --rm o-rly [args...]
```

## Architecture

### Relay Core: ORelay

`ORelay` is a hard fork of moxygen's `MoQRelay`. We copy the relay core into
o-rly so we can evolve it independently (threading model, custom cache miss
handling, chained caches, etc.) while still using moxygen's lower-level
building blocks as libraries:

- **MoQForwarder** — fan-out engine, used as-is from moxygen for now. May need
  to fork in the future to accommodate threading model differences.
- **MoQCache** — object cache, used as-is from moxygen. Custom miss handling
  and chained cache support may be upstreamed to openmoq/moxygen or maintained
  in our fork.
- **MoQSession / MoQServer / MoQRelaySession** — session and server
  infrastructure, used as libraries.

### ORelayServer

`ORelayServer` extends `MoQServer` to wire up `ORelay` as the publish/subscribe
handler and create `MoQRelaySession` instances for incoming connections.

## Design

- `design/ARCHITECTURE.md`
- `design/ROADMAP.md`
