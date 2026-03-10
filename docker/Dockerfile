FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        libfmt9 \
        libunwind8 \
    && rm -rf /var/lib/apt/lists/*
COPY install/bin/o_rly /usr/local/bin/o-rly
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
