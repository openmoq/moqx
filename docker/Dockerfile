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

# MoQX source — only what cmake needs
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake/ cmake/
COPY src/ src/
COPY deps/catapult/ deps/catapult/
COPY deps/moxygen/build/fbcode_builder/CMake deps/moxygen/build/fbcode_builder/CMake

# The build context excludes .git, so cmake cannot derive the version here;
# CI passes it in. Empty falls back to the static project version.
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

# Same value the binary reports, as image metadata: `docker inspect` and
# registry UIs can identify an image without pulling or running it.
# ARGs do not cross stages, so it is redeclared here.
ARG MOQX_VERSION_STRING=""
LABEL org.opencontainers.image.title="moqx" \
      org.opencontainers.image.description="MoQ relay" \
      org.opencontainers.image.version="${MOQX_VERSION_STRING}" \
      org.opencontainers.image.source="https://github.com/openmoq/moqx" \
      org.opencontainers.image.licenses="Apache-2.0"

COPY --from=builder /install/bin/moqx /usr/local/bin/moqx
COPY --from=builder /install/bin/moqx-issuer /usr/local/bin/moqx-issuer
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
COPY docker/config.docker.yaml /usr/local/share/moqx/config.docker.yaml
RUN chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
