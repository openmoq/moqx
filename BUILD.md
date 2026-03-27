# Building and Running o-rly

## Prerequisites

- CMake 3.25+ ([Kitware repo](https://apt.kitware.com/) if Ubuntu apt gives < 3.25)
- Ninja, C++20 compiler (GCC 11+ / Clang 14+)
- `gh` CLI (authenticated, for downloading release artifacts)
- System libraries — `build.sh setup` checks and reports what's missing

Install system deps (Ubuntu/Debian):

```bash
sudo deps/moxygen/standalone/install-system-deps.sh
```

## Quick Start

```bash
git clone https://github.com/openmoq/o-rly.git && cd o-rly
git submodule update --init
sudo deps/moxygen/standalone/install-system-deps.sh

./scripts/build.sh setup      # download prebuilt moxygen deps (~1 min)
./scripts/build.sh            # configure + build
./scripts/build.sh test       # run tests (77 tests, <1s)
```

## Dependency Modes

o-rly depends on moxygen (and its Meta deps: folly, fizz, wangle, mvfst, proxygen).
The `deps/moxygen` submodule pins the exact version. Two ways to get these deps:

| Mode | Command | Time | When to use |
|------|---------|------|-------------|
| **from-release** | `build.sh setup` | ~1 min | Default — downloads CI-built artifacts |
| **from-source** | `build.sh setup --from-source` | 15-30 min | Full control, or when artifacts unavailable |

Both accept an optional commit SHA to override the submodule pointer:

```bash
build.sh setup --from-release abc1234   # artifacts for specific moxygen commit
build.sh setup --from-source abc1234    # build specific commit from source
```

To build against a local moxygen checkout (for iterating on moxygen itself):

```bash
build.sh setup --from-source --moxygen-dir ~/src/moxygen
```

Default (no SHA or dir) uses the current submodule HEAD.
Falls back from release to source if artifacts aren't available.
Use `--no-fallback` to fail instead. Use `--clean` to wipe `.scratch/` first.

## Build Profiles

```
build.sh setup [--from-release [SHA]|--from-source [SHA]] [--moxygen-dir DIR] [--no-fallback] [--clean]
build.sh [--profile default|san] [--build-dir DIR]
build.sh test [--build-dir DIR] [-- CTEST_ARGS...]
```

| Profile | Build dir | Description |
|---------|-----------|-------------|
| `default` | `build/` | RelWithDebInfo |
| `san` | `build-san/` | Debug + ASAN/UBSAN |

## Formatting and Linting

CI requires clang-format-19. Check before pushing:

```bash
./scripts/format.sh --check    # verify (dry-run)
./scripts/format.sh            # fix in-place
./scripts/lint.sh build        # clang-tidy (requires prior build)
```

## PR Process

1. Create a branch, push changes
2. CI runs: format check + build/test (default + ASAN)
3. All checks must pass before merge
4. Squash-and-merge preferred for single-feature PRs

## Docker Image

The relay is published as a Docker image on every push to main:

```bash
docker pull ghcr.io/openmoq/o-rly:latest
```

### Running the Relay

The Docker compose file handles TLS certs, logging, health checks, and log viewing:

```bash
cd docker
cp .env.example .env          # edit with your domain and settings
docker compose up -d           # starts relay + dozzle log viewer
```

Key environment variables (set in `.env`):

| Variable | Default | Description |
|----------|---------|-------------|
| `DOMAIN` | — | TLS certificate domain |
| `ORLY_PORT` | `4433` | QUIC/UDP listen port |
| `ORLY_ADMIN_PORT` | `8000` | Admin HTTP port (localhost only) |
| `ORLY_BIND_ADDR` | `0.0.0.0` | Listen address (`0.0.0.0` or `::`) |
| `ORLY_MAX_TRACKS` | `1000` | Max cached tracks |
| `ORLY_MAX_GROUPS` | `100` | Max groups per track in cache |
| `ORLY_LOG_LEVEL` | `0` | Min log level: 0=INFO 1=WARNING 2=ERROR 3=FATAL |
| `ORLY_VERBOSE` | `0` | GLOG verbose level: 0=off, 1-4=increasing detail |
| `ORLY_INSECURE` | `false` | Use built-in dev cert (local testing only) |
| `ORLY_ENTRY` | `./entrypoint.sh` | Custom entrypoint override path |

### TLS Certificate Provisioning

For Route53 DNS challenge:

```bash
# Add AWS creds to .env, then:
docker compose --profile certbot-route53 run --rm certbot-route53
```

For Cloudflare DNS challenge:

```bash
cp docker/cloudflare.ini.example docker/cloudflare.ini  # fill in API token
docker compose --profile certbot-cloudflare run --rm certbot-cloudflare
```

### Health Check

The relay exposes an admin HTTP endpoint:

```bash
curl http://localhost:8000/info
# {"service":"o-rly","version":"0.1.0"}
```

Docker compose includes a health check that polls this endpoint. If the relay becomes
unresponsive, `restart: unless-stopped` triggers automatic recovery.

### Log Viewer (Dozzle)

The compose file includes [Dozzle](https://dozzle.dev/) for browsing relay logs
in a web UI. It starts automatically with the relay, bound to `127.0.0.1:9999`.

Access on a remote host via SSH tunnel:

```bash
ssh -L 19999:localhost:9999 user@relay-host
# Then browse http://localhost:19999
```

The local port (19999) can be any available port on your machine.

### Diagnosing Relay Issues

Check UDP socket health (drops indicate the relay can't keep up):

```bash
docker exec moqx cat /proc/net/udp | grep 1151
```

Fields to watch:
- `drops` (last column) — packets discarded by the socket
- `tx_queue` (5th column) — bytes backed up in the send buffer

High drops + backed up tx_queue = relay is overloaded or stuck:

```bash
docker restart moqx
```

## CI and Automation

See [design/ci-architecture.md](design/ci-architecture.md) for the full CI pipeline:
upstream sync, submodule updates, build/publish/release, and auto-deploy.
