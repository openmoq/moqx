# ── Build stage: compile o-rly inside bookworm against moxygen tarball ───
FROM debian:bookworm AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ninja-build git ca-certificates python3-pip \
        libssl-dev libunwind-dev libgoogle-glog-dev libgflags-dev \
        libdouble-conversion-dev libevent-dev libsodium-dev libzstd-dev \
        libboost-all-dev libfmt-dev zlib1g-dev libc-ares-dev libdwarf-dev \
    && pip install --break-system-packages cmake \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Moxygen tarball (pre-extracted bookworm build)
COPY .docker-deps/moxygen /opt/moxygen

# O-rly source — only what cmake needs
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake/ cmake/
COPY src/ src/
COPY include/ include/
COPY deps/moxygen/build/fbcode_builder/CMake deps/moxygen/build/fbcode_builder/CMake

RUN cmake -S . -B _build --preset default \
        -DCMAKE_PREFIX_PATH=/opt/moxygen \
        -DBUILD_TESTING=OFF -DORLY_BUILD_TESTS=OFF \
    && cmake --build _build -j$(nproc) \
    && cmake --install _build --prefix /install

# ── Runtime stage: minimal image with just the binary ────────────────────
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libunwind8 libsodium23 libboost-context1.74.0 \
        libgoogle-glog0v5 libgflags2.2 libdouble-conversion3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /install/bin/o_rly /usr/local/bin/o-rly
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
