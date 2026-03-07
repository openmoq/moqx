FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libssl3 && rm -rf /var/lib/apt/lists/*
COPY install/bin/o_rly /usr/local/bin/o-rly
ENTRYPOINT ["o-rly"]
