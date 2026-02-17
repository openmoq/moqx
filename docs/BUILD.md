# Build system

This repo uses a simple CMake + scripts layout that works on Linux/macOS and is reproducible on Ubuntu 20.04 via Docker.

## Directory layout

- `CMakeLists.txt` / `CMakePresets.json`: project entry points.
- `cmake/`: shared CMake modules (lint/format, etc.).
- `include/`: public headers.
- `src/`: library and binary sources.
- `tests/`: unit/integration tests.
- `tools/`: helper binaries (reserved).
- `scripts/`: common build/test helpers.
- `docker/`: Ubuntu 20 build environment.
- `docs/`: contributor documentation.

## Quick start (native)

```bash
./scripts/configure.sh
./scripts/build.sh
./scripts/test.sh
```

## Presets (recommended)

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

## Sanitizers

```bash
cmake --preset san
cmake --build --preset san
```

## Linting and formatting

Requirements: `clang-format` and `clang-tidy` installed locally.

```bash
./scripts/format.sh
./scripts/lint.sh build
```

CMake targets are also available when tools are installed:

```bash
cmake --build build --target format
cmake --build build --target lint
```

The CMake targets discover files dynamically under `include/`, `src/`, `tests/`, and `tools/`.

## Docker (Ubuntu 20.04 baseline)

```bash
./scripts/docker-build.sh
./scripts/docker-shell.sh
```

Inside the container:

```bash
./scripts/configure.sh
./scripts/build.sh
./scripts/test.sh
```

## Notes

- C++ standard is set to C++20.
- `ORLY_ENABLE_SANITIZERS=ON` enables ASAN/UBSAN for non-Release builds.
- The build defaults to `RelWithDebInfo` to keep symbols available.
