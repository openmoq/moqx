# ── Build stage: compile moqx inside bookworm against moxygen tarball ───
FROM debian:bookworm AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates \
        libssl-dev libunwind-dev libgoogle-glog-dev libgflags-dev \
        libdouble-conversion-dev libevent-dev libsodium-dev libzstd-dev \
        libboost-all-dev libfmt-dev zlib1g-dev libc-ares-dev libdwarf-dev libaio-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Moxygen tarball (pre-extracted bookworm build)
COPY .docker-deps/moxygen /opt/moxygen

# MoQX source — only what cmake needs.
# VERSIO[N] is an optional-copy glob: CI stamps a VERSION file into the build
# context (the context excludes .git, so cmake can't run git describe here),
# but a bare `docker build` has none and must still work. The glob matches
# nothing in that case, and the COPY still succeeds because CMakeLists.txt
# does — a plain `COPY VERSION` would hard-fail instead.
COPY CMakeLists.txt CMakePresets.json VERSIO[N] ./
COPY cmake/ cmake/
COPY src/ src/
COPY deps/catapult/ deps/catapult/
COPY deps/moxygen/build/fbcode_builder/CMake deps/moxygen/build/fbcode_builder/CMake

# Explicit override for one-off builds; normally empty, in which case cmake
# falls through to the VERSION file copied above (then to the static project
# version if that is absent too). See cmake/MoqxVersion.cmake.
ARG MOQX_VERSION_STRING=""

RUN cmake -S . -B _build --preset default \
        -DCMAKE_PREFIX_PATH=/opt/moxygen \
        -DMOQX_VERSION_STRING="${MOQX_VERSION_STRING}" \
        -DBUILD_TESTING=OFF -DMOQX_BUILD_TESTS=OFF \
    && cmake --build _build -j$(nproc) \
    && cmake --install _build --prefix /install

# ── Runtime stage: minimal image with just the binary ────────────────────
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libunwind8 libsodium23 libboost-context1.74.0 \
        libgoogle-glog0v6 libgflags2.2 libdouble-conversion3 \
        libjemalloc2 \
        curl gettext-base \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /install/bin/moqx /usr/local/bin/moqx
COPY --from=builder /install/bin/moqx-issuer /usr/local/bin/moqx-issuer
# Build identifier as a plain file, so an image can be identified without
# executing it — inspecting a layer, or a foreign-arch image that cannot run
# here. `docker run --rm <image> moqx --version` reports the same string.
COPY --from=builder /install/VERSION /usr/local/VERSION
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
COPY docker/config.docker.yaml /usr/local/share/moqx/config.docker.yaml
RUN chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
