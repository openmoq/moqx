# Deploying and Configuring Relay Hops

moqx implements `draft-lcurley-moq-relay-hops` to prevent namespace
advertisements from circulating indefinitely through a relay cycle. Supporting
peers negotiate the extension independently on each MOQT session, attach an
origin-to-relay `HOP_PATH` to namespace advertisements, and drop an
advertisement when the local Hop ID is already in that path.

The repository copy of
[`draft-lcurley-moq-relay-hops`](draft-lcurley-moq-relay-hops.txt) is the
normative reference. This guide describes the current moqx deployment and
configuration behavior.

## Deployment Requirements

Every moqx binary in a protected relay cycle must contain both parts of the
implementation:

- the moqx relay policy and upstream behavior;
- the pinned moxygen wire-format and per-session negotiation support.

The `deps/moxygen` submodule records the required moxygen revision. Initialize
submodules before building:

```bash
git submodule update --init --recursive
```

For a source deployment, stage the pinned dependency, build moqx, validate the
configuration, and then start the relay:

```bash
sudo deps/moxygen/standalone/install-system-deps.sh
./scripts/build.sh setup
./scripts/build.sh
./build/moqx validate-config --config /etc/moqx/config.yaml
./build/moqx --config /etc/moqx/config.yaml
```

`build.sh setup` downloads the release artifact matching the pinned moxygen
revision and falls back to a source build when that artifact is unavailable. To
require the checked-out moxygen source, use:

```bash
./scripts/build.sh setup --from-source --no-fallback
./scripts/build.sh
```

See [`BUILD.md`](../BUILD.md) for platform prerequisites and dependency modes,
and [`RUNNING.md`](../RUNNING.md) for process, logging, and container options.

### Container Images

A container image must be built from a moqx revision that pins a
relay-hops-capable moxygen revision. Do not combine a feature-enabled moqx
checkout with an older moxygen artifact.

The project image build expects a Debian Bookworm moxygen bundle in
`.docker-deps/moxygen`. CI selects the `bookworm-amd64` or `bookworm-arm64`
artifact, stages it there, and builds `docker/Dockerfile`. Use that same flow for
a custom image; a dependency bundle built for another distribution may not be
ABI-compatible with the Bookworm build stage.

When using a published image, confirm that its moqx revision includes the relay
hops implementation before introducing a cycle. Run it with a mounted
configuration:

```bash
docker run --rm \
  --name moqx \
  --restart no \
  -v /etc/moqx/config.yaml:/etc/moqx/config.yaml:ro \
  -v /etc/moqx/tls:/etc/moqx/tls:ro \
  -p 4433:4433/udp \
  -p 127.0.0.1:8000:8000/tcp \
  ghcr.io/openmoq/moqx:<verified-tag>
```

For a long-running service, apply the site's normal restart policy or service
manager after validating the image and configuration.

## Configuration Model

Relay hops negotiation is automatic. There is no `relay_hops` YAML field and no
feature toggle.

At process startup, moqx creates one random Hop ID for the relay context. The ID
is stable for that process lifetime and shared by its services and sessions. It
is regenerated after a restart. The existing top-level `relay_id` setting is an
operational relay identity; it does not set or expose the protocol Hop ID.

On each peer session:

- moqx advertises the zero-length `RELAY_HOPS` setup option;
- the extension becomes active only when both peers advertise support;
- negotiated advertisements carry `HOP_PATH`;
- moqx appends its Hop ID before forwarding an advertisement;
- moqx drops an advertisement whose incoming path already contains its Hop ID;
- the wildcard upstream namespace subscription carries `EXCLUDE_HOP` for the
  local Hop ID.

Negotiation is session-local. Support on one peer connection does not imply
support on any other connection.

## Configure a Root Relay

A root relay has no `upstream` block. The following example accepts raw MOQT and
WebTransport traffic on UDP port 4433 and exposes the admin API only on
localhost:

```yaml
relay_id: "relay-root"
threads: 4

listeners:
  - name: main
    udp:
      socket:
        address: "0.0.0.0"
        port: 4433
    tls:
      cert_file: "/etc/moqx/tls/fullchain.pem"
      key_file: "/etc/moqx/tls/privkey.pem"
      insecure: false
    endpoint: "/moq-relay"
    moqt_versions: [16, 18]

services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache:
      enabled: true
      max_tracks: 1000
      max_groups_per_track: 100

admin:
  address: "127.0.0.1"
  port: 8000
  plaintext: true
```

The feature is exercised by the draft-16 relay integration tests. The
implementation also includes the draft-17 and draft-18 hop-path encodings.
Configure at least one common supported MOQT version on every peer link.

## Configure a Relay with an Upstream

Add one `upstream` block to the service that should join the relay graph:

```yaml
relay_id: "relay-edge-a"
threads: 4

listeners:
  - name: main
    udp:
      socket:
        address: "0.0.0.0"
        port: 4433
    tls:
      cert_file: "/etc/moqx/tls/fullchain.pem"
      key_file: "/etc/moqx/tls/privkey.pem"
      insecure: false
    endpoint: "/moq-relay"
    moqt_versions: [16, 18]

services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache:
      enabled: true
      max_tracks: 1000
      max_groups_per_track: 100
    upstream:
      url: "moqt://relay-root.example.com:4433/moq-relay"
      tls:
        insecure: false
        # ca_cert: "/etc/moqx/tls/cluster-ca.pem"
      connect_timeout_ms: 5000
      idle_timeout_ms: 60000

admin:
  address: "127.0.0.1"
  port: 8000
  plaintext: true
```

Use `tls.insecure: true` only in isolated development or integration-test
networks. In production, use certificates trusted by the system trust store or
set `ca_cert` to the cluster CA. `ca_cert` and `insecure: true` are mutually
exclusive.

Upstream configuration is per service. Each service maintains at most one
upstream connection, reconnecting with backoff after a failure. Services do not
share namespace or hop-path state.

## Configure a Relay Cycle

A cycle is configured by pointing each service at the next relay. For example:

```text
relay-a -> relay-b -> relay-c -> relay-a
```

Each relay uses the same service shape shown above, with these upstream URLs:

```yaml
# relay-a
upstream:
  url: "moqt://relay-b.example.com:4433/moq-relay"
  tls:
    insecure: false
```

```yaml
# relay-b
upstream:
  url: "moqt://relay-c.example.com:4433/moq-relay"
  tls:
    insecure: false
```

```yaml
# relay-c
upstream:
  url: "moqt://relay-a.example.com:4433/moq-relay"
  tls:
    insecure: false
```

The shortened fragments belong under the same `services.<name>` entry on their
respective relays. Use fully qualified configurations in production.

All links in the cycle must negotiate relay hops. If any relay or link does not
preserve `HOP_PATH`, loop detection is no longer end-to-end. Keep every graph
segment containing a legacy or non-supporting relay acyclic.

## Rolling Upgrade Procedure

Do not add a cycle while unsupported relays remain in that cycle.

1. Keep the current topology acyclic.
2. Deploy the relay-hops-capable moxygen and moqx build to every relay that will
   participate.
3. Restart each process so it uses the new binary and generates its process
   Hop ID.
4. Verify each relay's health, upstream connectivity, and namespace propagation.
5. Update the upstream blocks to introduce the cycle.
6. Verify that namespace state reaches every relay and then remains stable.

If an older relay must remain in service, keep the portion of the graph that
crosses it acyclic. A supporting moqx relay assigns a stable random stand-in
origin ID to advertisements received from a non-negotiated source session, but
the legacy hop cannot carry path state through a cycle.

## Verify the Deployment

Validate each configuration before restart:

```bash
./build/moqx validate-config --strict_config --config /etc/moqx/config.yaml
```

After startup, check the local admin interface:

```bash
curl --fail http://127.0.0.1:8000/info
curl --fail http://127.0.0.1:8000/state
curl --fail http://127.0.0.1:8000/metrics
```

Publish a test namespace on one relay and confirm that it appears in the
`namespace_tree` returned by `/state` on every expected relay. Take a second
snapshot after a settle interval and confirm that the tree is unchanged.

For a source checkout, run the relay-chain and cycle integration tests:

```bash
ctest --test-dir build \
  --output-on-failure \
  -R '^(relay_chain|relay_hops_cycle)$'
```

For additional diagnostics, enable relay lifecycle logging:

```bash
./build/moqx --config /etc/moqx/config.yaml --logtostderr -v 2 \
  --vmodule "MoqxRelay=4,MoQSession=4"
```

A returning advertisement is dropped before it is registered or forwarded.
Malformed non-empty paths close the offending session with
`PROTOCOL_VIOLATION`; a negotiated advertisement missing `HOP_PATH` is dropped
without closing the session.

## Current Scope and Limitations

- There is no YAML feature flag or operator-configured Hop ID.
- Each service supports one upstream; multi-upstream path selection, failover,
  and load balancing are not implemented.
- Shortest-path selection, redundant-origin comparison, topology coalescing,
  and trust-boundary path stripping described as optional behavior in the draft
  are not implemented.
- Hop IDs are opaque routing-loop identifiers, not authentication or
  authorization credentials.
- `HOP_PATH` exposes path length. Protect peer links with TLS and consider this
  topology information when defining trust boundaries.

See the [configuration reference](config.md#relay-architectures) for the full
service and upstream schema.
