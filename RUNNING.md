# Running the Relay

## Command Line

```bash
moqx --config config.yaml
```

### Subcommands

| Subcommand | Description |
|------------|-------------|
| `serve` | Start the relay (default if omitted) |
| `validate-config` | Load and validate config, then exit |
| `dump-config-schema` | Print JSON schema to stdout, then exit |

### Flags

| Flag | Description |
|------|-------------|
| `--config <path>` | Path to config file (required) |
| `--strict_config` | Reject unknown fields in config |
| `--help` | Show all flags (including glog, folly, mvfst internals) |

See [config.example.yaml](config.example.yaml) for the full config schema.

moqx also inherits flags from glog, folly, and mvfst. The most useful
are the logging flags documented below. Run `moqx --help` for the full list.

## Logging

moqx uses glog (via folly). Every glog flag works as a CLI argument or
environment variable (`GLOG_<flag>=<value>`). CLI flags take precedence.

### Log Level (--minloglevel)

| Value | Level |
|-------|-------|
| 0 | INFO (default) |
| 1 | WARNING |
| 2 | ERROR |
| 3 | FATAL |

### Verbose Level (-v)

| Level | Content |
|-------|---------|
| 0 | Off (default) |
| 1 | Session/subscription lifecycle |
| 2 | Subscribe/publish operations, namespace management |
| 3 | Object forwarding, stream operations, error details |
| 4 | Per-packet/per-object detail, buffer accounting |

### Per-Module Verbosity (--vmodule)

Comma-separated `<glob>=<level>` pairs matched against filename (no path/extension).

```bash
moqx --config c.yaml --vmodule "MoQSession=4,MoQForwarder=4,MoqxRelay=4"
moqx --config c.yaml --vmodule "MoQ*=4,Quic*=2"
```

**moqx/moxygen modules:**

| Module | Content |
|--------|---------|
| `MoqxRelay` | Publish/subscribe lifecycle, namespace management |
| `MoQSession` | Stream open/close, STOP_SENDING, RESET_STREAM |
| `MoQForwarder` | Object forwarding, subgroup errors |
| `MoQServer` | Server setup, connection acceptance |
| `MoQFramer` | Message serialization/deserialization |

**mvfst (QUIC) modules:**

| Module | Content |
|--------|---------|
| `QuicTransportBase` | Flow control, stream state, congestion |
| `QuicStreamManager` | Stream creation, limits, state transitions |
| `QuicFlowControl` | MAX_DATA, MAX_STREAM_DATA decisions |
| `QuicCongestionControl` | Congestion window, loss detection |

### Log Destination

| Flag | Description |
|------|-------------|
| `--logtostderr` | All output to stderr |
| `--alsologtostderr` | Log to files and stderr |
| `--log_dir DIR` | Write log files to directory |
| `--stderrthreshold N` | Copy this level and above to stderr (default: 2) |

### Debugging

```bash
# Investigate stream resets
moqx --config c.yaml --logtostderr -v 2 \
  --vmodule "MoQSession=4,MoQForwarder=4,MoqxRelay=4"

# Full debug
moqx --config c.yaml --logtostderr -v 4

# Production with file logging
moqx --config c.yaml --log_dir /var/log/moqx --minloglevel 1

# Stack trace at a specific source line
moqx --config c.yaml --log_backtrace_at "MoQForwarder.cpp:819"
```

## Health Check

```bash
curl http://localhost:8000/info
# {"service":"moqx","version":"0.1.0"}
```

## Diagnosing Issues

Check UDP socket health (drops indicate the relay can't keep up):

```bash
cat /proc/net/udp | grep 1151    # or via docker exec
```

Fields: `drops` (last column) = packets discarded, `tx_queue` (5th column) = send backlog.

---

## Docker

### Image

```bash
docker pull ghcr.io/openmoq/moqx:latest
```

### Quick Start (Docker Compose)

```bash
cd docker
cp .env.example .env
docker compose up -d
```

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `DOMAIN` | -- | TLS certificate domain |
| `MOQX_PORT` | `4433` | QUIC/UDP listen port |
| `MOQX_ADMIN_PORT` | `8000` | Admin HTTP port |
| `MOQX_BIND_ADDR` | `0.0.0.0` | Listen address |
| `MOQX_MAX_TRACKS` | `1000` | Max cached tracks |
| `MOQX_MAX_GROUPS` | `100` | Max groups per track |
| `MOQX_LOG_LEVEL` | `0` | Min log level (0-3) |
| `MOQX_VERBOSE` | `0` | VLOG level (0-4) |
| `MOQX_LOG_PORT` | `9999` | Dozzle log viewer port (localhost only) |
| `GLOG_vmodule` | -- | Per-module verbose level (passed through) |
| `MOQX_INSECURE` | `false` | Use built-in dev cert |

The entrypoint maps `MOQX_LOG_LEVEL` and `MOQX_VERBOSE` to their `GLOG_*`
equivalents and forces `GLOG_logtostderr=1`.

### Custom Config File

The entrypoint auto-detects `/etc/moqx/config.yaml`. If present, env-var
config generation is skipped. Log-level env vars still apply.

```bash
docker run --rm \
  -v /path/to/config.yaml:/etc/moqx/config.yaml:ro \
  -v /etc/letsencrypt:/certs:ro \
  -p 4433:4433/udp \
  ghcr.io/openmoq/moqx:latest
```

### Docker Compose Debug Logging

```yaml
environment:
  MOQX_LOG_LEVEL: "0"
  MOQX_VERBOSE: "4"
  GLOG_vmodule: "MoQSession=4,MoQForwarder=4,MoqxRelay=4"
```

### TLS Certificates

**Route53:**
```bash
docker compose --profile certbot-route53 run --rm certbot-route53
```

**Cloudflare:**
```bash
cp docker/cloudflare.ini.example docker/cloudflare.ini
docker compose --profile certbot-cloudflare run --rm certbot-cloudflare
```

**Custom certs:** Set `MOQX_CERT` and `MOQX_KEY` in `.env`.

### Log Viewer (Dozzle)

Included in compose, bound to `127.0.0.1:9999`. Access remotely via SSH tunnel:

```bash
ssh -L 19999:localhost:9999 user@relay-host
# browse http://localhost:19999
```
