# Running the Relay

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

See [config.example.yaml](config.example.yaml) for the full config schema with
multi-service routing, cache tuning, and TLS options.

Validate a config without starting the server:

```bash
moqx validate-config --config /path/to/config.yaml
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

## Log Viewer (Dozzle)

The compose file includes [Dozzle](https://dozzle.dev/) for browsing relay logs
in a web UI. It starts automatically with the relay, bound to `127.0.0.1:9999`.

Access on a remote host via SSH tunnel:

```bash
ssh -L 19999:localhost:9999 user@relay-host
# Then browse http://localhost:19999
```

The local port (19999) can be any available port on your machine.

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
