# Running the Relay

## Command-Line Launch

Build the relay (see [BUILD.md](BUILD.md)), then run directly:

```bash
# Minimal: insecure mode with default config
moqx --config config.example.yaml

# With TLS certs
moqx --config /path/to/config.yaml

# Validate config without starting
moqx validate-config --config /path/to/config.yaml
```

See [config.example.yaml](config.example.yaml) for the full config schema with
multi-service routing, cache tuning, transport settings, and TLS options.

## Docker Image

The relay is published as a Docker image on every push to main:

```bash
docker pull ghcr.io/openmoq/moqx:latest
```

## Quick Start (Docker Compose)

```bash
cd docker
cp .env.example .env          # edit with your domain and settings
docker compose up -d           # starts relay + dozzle log viewer
```

## Environment Variables

The default entrypoint generates a relay config from these env vars:

| Variable | Default | Description |
|----------|---------|-------------|
| `DOMAIN` | -- | TLS certificate domain |
| `MOQX_PORT` | `4433` | QUIC/UDP listen port |
| `MOQX_ADMIN_PORT` | `8000` | Admin HTTP port (localhost only) |
| `MOQX_BIND_ADDR` | `0.0.0.0` | Listen address (`0.0.0.0` or `::`) |
| `MOQX_MAX_TRACKS` | `1000` | Max cached tracks |
| `MOQX_MAX_GROUPS` | `100` | Max groups per track in cache |
| `MOQX_LOG_LEVEL` | `0` | Min log level: 0=INFO 1=WARNING 2=ERROR 3=FATAL |
| `MOQX_VERBOSE` | `0` | GLOG verbose level: 0=off, 1-4=increasing detail |
| `MOQX_INSECURE` | `false` | Use built-in dev cert (local testing only) |
| `MOQX_ENTRY` | `./entrypoint.sh` | Custom entrypoint override path |

## Using a Custom Config File

The entrypoint auto-detects a config file at `/etc/moqx/config.yaml`.
If present, it uses that directly instead of generating one from env vars.
Log-level env vars (`MOQX_LOG_LEVEL`, `MOQX_VERBOSE`) still apply.

```bash
docker run --rm \
  -v /path/to/your/config.yaml:/etc/moqx/config.yaml:ro \
  -v /etc/letsencrypt:/certs:ro \
  -p 4433:4433/udp \
  ghcr.io/openmoq/moqx:latest
```

Or with docker compose, add a volume in `docker-compose.override.yml`:

```yaml
services:
  moqx:
    volumes:
      - /path/to/your/config.yaml:/etc/moqx/config.yaml:ro
```

## TLS Certificate Provisioning

### Route53 DNS Challenge

```bash
# Add AWS creds to .env, then:
docker compose --profile certbot-route53 run --rm certbot-route53
```

### Cloudflare DNS Challenge

```bash
cp docker/cloudflare.ini.example docker/cloudflare.ini  # fill in API token
docker compose --profile certbot-cloudflare run --rm certbot-cloudflare
```

### Self-Signed or Custom Certs

Set `MOQX_CERT` and `MOQX_KEY` in `.env` to point to your mounted cert files,
or use a custom config file (see above).

## Health Check

The relay exposes an admin HTTP endpoint:

```bash
curl http://localhost:8000/info
# {"service":"moqx","version":"0.1.0"}
```

Docker compose includes a health check that polls this endpoint. If the relay becomes
unresponsive, `restart: unless-stopped` triggers automatic recovery.

## Logging

moqx uses Google's glog library (via folly) for all logging. Every glog flag
is available as both a command-line argument and an environment variable.

| Method | Syntax | Example |
|--------|--------|---------|
| CLI flag | `--flag value` | `moqx --config c.yaml -v 4` |
| Environment | `GLOG_flag=value` | `export GLOG_v=4` |
| Docker env | `MOQX_*` mapped in entrypoint.sh | `MOQX_VERBOSE=4` |

CLI flags override environment variables. The Docker entrypoint maps
`MOQX_*` convenience vars to `GLOG_*` before exec.

### Log Level (--minloglevel / GLOG_minloglevel)

Controls the minimum severity for standard log messages.

| Value | Level | Description |
|-------|-------|-------------|
| 0 | INFO | All messages (default) |
| 1 | WARNING | Warnings and above |
| 2 | ERROR | Errors and fatal only |
| 3 | FATAL | Fatal only (crashes on FATAL) |

### Verbose Level (-v / GLOG_v)

Enables `VLOG(N)` messages for all modules where N <= this value.

| Level | Typical Content |
|-------|-----------------|
| 0 | Off (default) |
| 1 | High-level session/subscription lifecycle |
| 2 | Subscribe/publish operations, namespace management |
| 3 | Object forwarding, stream operations, error details |
| 4 | Per-packet/per-object detail, buffer accounting |

### Per-Module Verbose Level (--vmodule / GLOG_vmodule)

Overrides `-v` for specific source files. Comma-separated `<glob>=<level>` pairs
matched against the filename base (without path or extension).

```bash
# MoQT session + forwarder at max, everything else quiet
moqx --config c.yaml --vmodule "MoQSession=4,MoQForwarder=4,MoqxRelay=4"

# Glob patterns work
moqx --config c.yaml --vmodule "MoQ*=4,Quic*=2"
```

**Key moqx/moxygen modules:**

| Module | What it logs |
|--------|-------------|
| `MoqxRelay` | Relay: publish/subscribe lifecycle, namespace management |
| `MoQSession` | Session: stream open/close, STOP_SENDING, RESET_STREAM |
| `MoQForwarder` | Forwarder: object forwarding, subgroup errors |
| `MoQServer` | Server setup, connection acceptance |
| `MoQFramer` | Wire format: message serialization/deserialization |

**Key mvfst (QUIC) modules:**

| Module | What it logs |
|--------|-------------|
| `QuicTransportBase` | Connection: flow control, stream state, congestion |
| `QuicStreamManager` | Stream creation, limits, state transitions |
| `QuicFlowControl` | MAX_DATA, MAX_STREAM_DATA decisions |
| `QuicCongestionControl` | Congestion window, loss detection |

### Log Destination

| Flag | Description |
|------|-------------|
| `--logtostderr` | Send all output to stderr (Docker entrypoint forces this) |
| `--alsologtostderr` | Log to files AND stderr |
| `--log_dir DIR` | Write log files to this directory |
| `--stderrthreshold N` | Copy messages at this level or above to stderr (default: 2=ERROR) |

### Recommended Configurations

**Investigating stream resets / STOP_SENDING:**
```bash
moqx --config c.yaml --logtostderr -v 2 \
  --vmodule "MoQSession=4,MoQForwarder=4,MoqxRelay=4" \
  2>&1 | tee /tmp/moqx_debug.log
```

**Full debug (very noisy):**
```bash
moqx --config c.yaml --logtostderr -v 4
```

**Production with file logging:**
```bash
moqx --config c.yaml --log_dir /var/log/moqx --minloglevel 1
```

**Docker Compose debug:**
```yaml
environment:
  MOQX_LOG_LEVEL: "0"
  MOQX_VERBOSE: "4"
  GLOG_vmodule: "MoQSession=4,MoQForwarder=4,MoqxRelay=4"
```

### Debugging Aids

- `--log_backtrace_at "File.cpp:123"` — emit a stack trace at a specific source line
- `--symbolize_stacktrace` — symbolize crash stack traces (default: true)

## Log Viewer (Dozzle)

The compose file includes [Dozzle](https://dozzle.dev/) for browsing relay logs
in a web UI. It starts automatically with the relay, bound to `127.0.0.1:9999`.

Access on a remote host via SSH tunnel:

```bash
ssh -L 19999:localhost:9999 user@relay-host
# Then browse http://localhost:19999
```

## Diagnosing Relay Issues

Check UDP socket health (drops indicate the relay can't keep up):

```bash
docker exec moqx cat /proc/net/udp | grep 1151
```

Fields to watch:
- `drops` (last column) -- packets discarded by the socket
- `tx_queue` (5th column) -- bytes backed up in the send buffer

High drops + backed up tx_queue = relay is overloaded or stuck:

```bash
docker restart moqx
```
