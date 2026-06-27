# moqx scripts

Helper scripts for running and benchmarking a moqx relay. Run them from the
repository root.

## Relay quickstart

Run a relay with [`moqx-run.sh`](moqx-run.sh). It fills
[`config.bench.yaml`](config.bench.yaml) (a template with sensible defaults)
and serves it. Override anything with a flag or env var — you rarely need to.

### Simplest possible

```bash
# local dev relay — built-in self-signed cert, no root needed
./scripts/moqx-run.sh --insecure --no-sudo
```

Listens on **udp/4433** (MoQT), admin HTTP on **8000**, WebTransport endpoint
**`/moq-relay`**. Offers MoQT drafts **16, 14, 18** (negotiates 16 with most
clients; draft-18-only clients get 18).

### The handful of commands you'll actually use

```bash
# 1. Local dev (insecure, no sudo)
./scripts/moqx-run.sh --insecure --no-sudo

# 2. Real TLS — Let's Encrypt cert under DOMAIN (sudo, to read the 0600 key)
DOMAIN=relay.example.com ./scripts/moqx-run.sh

# 3. Pin a single draft (e.g. force draft-18)
./scripts/moqx-run.sh --insecure --no-sudo --moqt-versions 18

# 4. Different port / endpoint
./scripts/moqx-run.sh --insecure --no-sudo --port 5433 --endpoint /relay

# 5. Validate the config without serving
./scripts/moqx-run.sh --subcmd validate-config --no-sudo

# 6. Show the resolved config + exact command, run nothing
./scripts/moqx-run.sh -n
```

### Defaults (all overridable)

| knob | default | flag / env |
|---|---|---|
| QUIC stack | mvfst | `--quic-stack mvfst\|picoquic` |
| listen port | 4433 | `--port` |
| admin port | 8000 | `--admin-port` |
| endpoint | `/moq-relay` | `--endpoint` |
| MoQT drafts | 16,14,18 (server-pref order) | `--moqt-versions` |
| IO threads | 4 | `--threads` |
| congestion control | bbr | `--cc` |
| object cache | on | `--cache` / `--no-cache` |
| TLS | real cert under `DOMAIN`, else dev cert | `--insecure` / `--cert` / `--key` |

Full option list: `./scripts/moqx-run.sh --help`.

### Gotchas

- **picoquic needs a *real* cert** (no insecure dev cert): `--quic-stack picoquic`
  with `DOMAIN=...` (or `--cert/--key`).
- `--no-sudo` is fine for insecure local runs; real TLS under `/etc/letsencrypt`
  needs sudo to read the private key.
- Check the relay is up: `curl http://localhost:8000/info`.

### Logging (if you need it)

- `-x ".=DBG4"` — verbose for **moqx's own** components (folly XLOG).
- `-v 2` — verbose for the **QUIC/HTTP stack** underneath (mvfst/proxygen, glog).

## Other scripts

- [`perf-test.sh`](perf-test.sh) — relay throughput / subscriber-ramp perf test
  (drives the relay via `moqx-run.sh`). See `./scripts/perf-test.sh` header for
  options; short flags `-s`/`-d`/`-t`/`-l`/`-j` mirror the common ones.
- [`config.bench.yaml`](config.bench.yaml) — the relay config template
  `moqx-run.sh` renders.
