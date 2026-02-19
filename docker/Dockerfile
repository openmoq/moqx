FROM debian:13 AS base

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    git \
    ca-certificates \
    python3 \
    curl \
    pkg-config \
    libssl-dev \
    m4 \
 && rm -rf /var/lib/apt/lists/*

# Stage 1: Build third-party deps only (cached until manifests change)
FROM base AS deps
COPY deps/moxygen/build/fbcode_builder /tmp/fbcode_builder
RUN python3 /tmp/fbcode_builder/getdeps.py build \
    --only-deps --no-tests \
    --scratch-path /opt/moxygen-scratch \
    --extra-cmake-defines '{"CMAKE_FIND_LIBRARY_SUFFIXES": ".a", "BUILD_SHARED_LIBS": "OFF"}' \
    moxygen

# Stage 2: Build moxygen (rebuilds on moxygen source changes)
FROM deps AS moxygen
COPY deps/moxygen /tmp/moxygen
RUN python3 /tmp/moxygen/build/fbcode_builder/getdeps.py build \
    --src-dir="moxygen:/tmp/moxygen" \
    --scratch-path /opt/moxygen-scratch \
    --extra-cmake-defines '{"CMAKE_FIND_LIBRARY_SUFFIXES": ".a", "BUILD_SHARED_LIBS": "OFF"}' \
    --no-tests \
    moxygen \
 && python3 /tmp/moxygen/build/fbcode_builder/getdeps.py show-inst-dir \
    --scratch-path /opt/moxygen-scratch \
    --extra-cmake-defines '{"CMAKE_FIND_LIBRARY_SUFFIXES": ".a", "BUILD_SHARED_LIBS": "OFF"}' \
    --recursive moxygen 2>/dev/null \
    | grep "^/" | tr '\n' ';' \
    > /opt/moxygen-scratch/cmake_prefix_path.txt

# Stage 3: Build o-rly (rebuilds on o-rly source changes)
FROM moxygen AS build
COPY . /workspace
WORKDIR /workspace
RUN cmake -S . -B build \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_PREFIX_PATH="$(cat /opt/moxygen-scratch/cmake_prefix_path.txt)" \
 && cmake --build build

# Stage 4: Minimal runtime image
FROM debian:13-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3t64 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /workspace/build/o_rly /usr/local/bin/o-rly
ENTRYPOINT ["o-rly"]
