# Targets: `relay` (default) and `interop-client`. Both consume the one moxygen
# prefix built by the `moxygen` stage, so the two images can never disagree about
# which moxygen they carry.
#
# ── moxygen prefix ───────────────────────────────────────────────────────
# Keyed on the pin and the build system, never on src/, so a source-only commit
# reuses this layer and only a MOXYGEN_REV bump pays for it. That is the whole
# reason moqx's sources are copied in the stage below and not this one.
FROM debian:bookworm AS moxygen

WORKDIR /src

# System libraries through the shared installer, so a new moqx dependency needs
# no second package list here. Image-only extras after it: ca-certificates for
# the configure-time downloads, libdwarf/libaio for the prebuilt proxygen link.
COPY scripts/install-system-deps.sh scripts/
RUN scripts/install-system-deps.sh \
    && apt-get install -y --no-install-recommends \
        ca-certificates libdwarf-dev libaio-dev \
    && rm -rf /var/lib/apt/lists/*

COPY cmake/ cmake/
COPY superbuild/ superbuild/

# The ladder `scripts/configure.sh --moxygen prebuilt-with-fallback` runs, driven
# here by the standalone fetch: configure.sh needs src/ and this stage must not.

# The token is a secret mount, not an ARG, which image history records. Optional —
# it only buys GitHub API rate limit (cmake/FetchMoxygenPrebuilt.cmake).
RUN --mount=type=cache,target=/root/.cache/moqx \
    --mount=type=secret,id=github_token \
    set -eu; \
    export MOQX_DEPS_CACHE=/root/.cache/moqx; \
    export GITHUB_TOKEN="$(cat /run/secrets/github_token 2>/dev/null || true)"; \
    if cmake -DOUT=/tmp/prefix -P cmake/fetch-moxygen-prebuilt.cmake; then \
        echo "==> moxygen: published prebuilt for the pin"; \
    else \
        echo "==> moxygen: no prebuilt for the pin — compiling it"; \
        cmake -S superbuild -B /tmp/sb -G Ninja; \
        CMAKE_BUILD_PARALLEL_LEVEL="$(nproc)" cmake --build /tmp/sb -j"$(nproc)"; \
        printf %s /tmp/sb/moxygen-install > /tmp/prefix; \
    fi; \
    cp -a "$(cat /tmp/prefix)" /opt/moxygen

# ── moqx ─────────────────────────────────────────────────────────────────
FROM moxygen AS builder

# The build context excludes .git, so cmake cannot derive the version here;
# CI passes it in. Empty falls back to the static project version.
ARG MOQX_VERSION_STRING=""

# PREBUILT=OFF so a prefix that failed to arrive is an error here rather than a
# silent second download that ignores the stage above.
COPY CMakeLists.txt CMakePresets.json ./
COPY src/ src/
RUN --mount=type=cache,target=/root/.cache/moqx \
    MOQX_DEPS_CACHE=/root/.cache/moqx \
    cmake -S . -B _build --preset default \
        -DMOQX_VERSION_STRING="${MOQX_VERSION_STRING}" \
        -DMOQX_BUILD_TESTS=OFF \
        -DMOQX_MOXYGEN_PREBUILT=OFF \
        -DCMAKE_PREFIX_PATH=/opt/moxygen \
    && cmake --build _build -j$(nproc) \
    && cmake --install _build --prefix /install

# ── interop client: moxygen's own binary, no moqx in the image ───────────
# Env-driven (RELAY_URL, TESTCASE, TLS_DISABLE_VERIFY, VERBOSE) to match the
# moq-interop-runner convention, so it needs no entrypoint wrapper.
FROM debian:bookworm-slim AS interop-client

RUN apt-get update && apt-get install -y --no-install-recommends \
        libunwind8 libsodium23 libboost-context1.74.0 \
        libgoogle-glog0v6 libgflags2.2 libdouble-conversion3 \
        libevent-2.1-7 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=moxygen /opt/moxygen/bin/moq_interop_client /usr/local/bin/moq_interop_client

# Non-root, UID 1000 by interop-runner convention.
RUN useradd -u 1000 -m interop
USER interop

ENTRYPOINT ["moq_interop_client"]

# ── relay: the published moqx image. Last, so it is the default target. ──
FROM debian:bookworm-slim AS relay

RUN apt-get update && apt-get install -y --no-install-recommends \
        libunwind8 libsodium23 libboost-context1.74.0 \
        libgoogle-glog0v6 libgflags2.2 libdouble-conversion3 \
        libevent-2.1-7 \
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
