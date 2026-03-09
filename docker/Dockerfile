FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        libboost-context1.74.0 \
        libdouble-conversion3 \
        libevent-2.1-7 \
        libfmt8 \
        libgflags2.2 \
        libgoogle-glog0v5 \
        libsodium23 \
        libssl3 \
        libunwind8 \
        libzstd1 \
    && rm -rf /var/lib/apt/lists/*
COPY install/bin/o_rly /usr/local/bin/o-rly
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
