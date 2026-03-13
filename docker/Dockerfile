# Multi-stage build: compile in bookworm, run in bookworm-slim.
#
# The CI workflow pre-downloads the moxygen bookworm tarball into
# .docker-deps/ before running docker build, so we COPY it in rather
# than needing gh CLI inside the container.
#
#   docker build -f docker/Dockerfile .

# ── Stage 1: Build ────────────────────────────────────────────────────────────

FROM debian:bookworm AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        git \
        ca-certificates \
        libssl-dev \
        libunwind-dev \
        libgoogle-glog-dev \
        libgflags-dev \
        libdouble-conversion-dev \
        libevent-dev \
        libsodium-dev \
        libzstd-dev \
        libboost-all-dev \
        libfmt-dev \
        zlib1g-dev \
        libc-ares-dev \
        gperf \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Unpack moxygen tarball (pre-downloaded by CI)
COPY .docker-deps/moxygen-bookworm-amd64.tar.gz /tmp/moxygen.tar.gz
RUN mkdir -p /opt/moxygen && tar xzf /tmp/moxygen.tar.gz -C /opt/moxygen

# Copy o-rly source (use .dockerignore to exclude build dirs)
COPY . /build

RUN cmake -S /build -B /build/_build -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_FIND_LIBRARY_SUFFIXES=".a" \
        -DCMAKE_PREFIX_PATH=/opt/moxygen \
        -DBUILD_TESTING=OFF \
    && cmake --build /build/_build -j"$(nproc)" \
    && cmake --install /build/_build --prefix /staging

# ── Stage 2: Runtime ──────────────────────────────────────────────────────────

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libfmt9 \
        libunwind8 \
        libdouble-conversion3 \
        libevent-2.1-7 \
        libgflags2.2 \
        libgoogle-glog0v6 \
        libsodium23 \
        libboost-context1.74.0 \
        libssl3 \
        zlib1g \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /staging/bin/o_rly /usr/local/bin/o-rly
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
