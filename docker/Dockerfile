# ── Build stage: compile moqx inside bookworm against moxygen tarball ───
FROM debian:bookworm AS builder

WORKDIR /src

# System libraries through the shared installer, so a new moqx dependency needs
# no second package list here. Image-only extras after it: ca-certificates for
# the configure-time downloads, libdwarf/libaio for the prebuilt proxygen link.
COPY scripts/install-system-deps.sh scripts/
RUN scripts/install-system-deps.sh \
    && apt-get install -y --no-install-recommends \
        ca-certificates libdwarf-dev libaio-dev \
    && rm -rf /var/lib/apt/lists/*

# MoQX source only — the configure fetches its own dependencies, so no dep trees
# are copied in. The bookworm base auto-detects the bookworm-<arch> platform tag,
# matching the published moxygen-bookworm-{amd64,arm64}.tar.gz prebuilts.
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake/ cmake/
COPY src/ src/

# The cache mount persists the fetched dependencies across image builds on the
# same builder (BuildKit), so a source-only change doesn't re-download ~680 MB.
# Harmless on cold/ephemeral builders.
RUN --mount=type=cache,target=/root/.cache/moqx \
    CPM_SOURCE_CACHE=/root/.cache/moqx/cpm \
    cmake -S . -B _build --preset default \
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
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
COPY docker/config.docker.yaml /usr/local/share/moqx/config.docker.yaml
RUN chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
