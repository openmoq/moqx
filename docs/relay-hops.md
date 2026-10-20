# Deploying and Configuring Relay Hops

moqx implements `draft-lcurley-moq-relay-hops` to prevent namespace
advertisements from circulating indefinitely through a relay cycle. Supporting
peers negotiate the extension independently on each MOQT session, attach an
origin-to-relay `HOP_PATH` to namespace advertisements, and drop an
advertisement when the local Hop ID is already in that path.

The repository copy of
[`draft-lcurley-moq-relay-hops`](ref/draft-lcurley-moq-relay-hops.txt) is the
normative reference. This guide describes the current moqx deployment and
configuration behavior.

## Deployment Requirements

Every moqx binary in a protected relay cycle must contain both parts of the
implementation:

- the moqx relay policy and upstream behavior;
- the pinned moxygen wire-format and per-session negotiation support.

The `deps/moxygen` submodule records the required revision. Build and runtime
instructions remain in [`BUILD.md`](../BUILD.md) and [`RUNNING.md`](../RUNNING.md).
Do not introduce a cycle using a moqx image paired with an older moxygen
artifact.

## Configuration Model

MoQ Cluster negotiation is on by default. The optional top-level `cluster`
block controls it:

```yaml
cluster:
  enabled: true   # false: do not declare HOP_ID, so no link negotiates MoQ Cluster
  hop_id: 1001    # absent: random per process; 0: anonymous
```

Disable it only to peer with a relay network that lacks MoQ Cluster or
implements it incompatibly. The operator must then keep that part of the graph
a tree; see [config.md](config.md#cluster).

The Hop ID is shared by all services and sessions of the process. A random one
changes after a restart. A relay with Hop ID 0 withholds its identity, so it
cannot detect loops through itself. The top-level `relay_id` setting is a
separate operational identity and does not set the Hop ID.

On each peer session:

- moqx declares its Hop ID in the `HOP_ID` setup option;
- the extension becomes active only when both peers declare a Hop ID;
- negotiated advertisements carry `HOP_PATH` and `ROUTE_COST`;
- moqx appends its Hop ID before forwarding an advertisement;
- moqx drops an advertisement whose incoming path already contains its Hop ID;
- moqx does not advertise a path to a peer whose Hop ID is on that path.

Negotiation is session-local. Support on one peer connection does not imply
support on any other connection.

## Configure a Relay Mesh

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
respective relays. The complete listener, TLS, service, and upstream schemas are
in [the configuration reference](config.md#relay-architectures). A service
peers with one `upstream` or with each entry of an `upstreams` list,
reconnecting each link with backoff after a failure. Services do not share
namespace or hop-path state.

Use `tls.insecure: true` only in isolated development or integration-test
networks. In production, use certificates trusted by the system trust store or
set `ca_cert` to the cluster CA. `ca_cert` and `insecure: true` are mutually
exclusive.

Relay-to-relay links use MOQT draft 18. Every relay that accepts upstream
connections must include 18 in its listener `moqt_versions`.

All links in the cycle must negotiate relay hops. If any relay or link does not
preserve `HOP_PATH`, loop detection is no longer end-to-end. Keep every graph
segment containing a relay without MoQ Cluster acyclic.

## Rolling Upgrade Procedure

Do not add a cycle while unsupported relays remain in that cycle.

1. Keep the current topology acyclic.
2. Deploy the relay-hops-capable moxygen and moqx build to every relay that will
   participate.
3. Restart each process so it uses the new binary.
4. Verify each relay's health, upstream connectivity, and namespace propagation.
5. Update the upstream blocks to introduce the cycle.
6. Verify that namespace state reaches every relay and then remains stable.

If an older relay must remain in service, keep the portion of the graph that
crosses it acyclic. A supporting moqx relay gives advertisements from a
non-negotiated source session the anonymous path `[0]`, but the older relay
cannot carry path state through a cycle.

## Verify the Deployment

After startup, check the relay state through the local admin interface:

```bash
curl --fail http://127.0.0.1:8000/state
```

Publish a test namespace on one relay and confirm that it appears in the
`namespace_tree` returned by `/state` on every expected relay. Take a second
snapshot after a settle interval and confirm that the tree is unchanged.

For a source checkout, run the relay-chain and cycle integration tests:

```bash
ctest --test-dir build \
  --output-on-failure \
  -R '^(relay_chain|relay_hops_cycle|relay_pyramid)$'
```

A returning advertisement is dropped before it is registered or forwarded.
A negotiated advertisement with a malformed or missing `HOP_PATH` closes the
offending session with `PROTOCOL_VIOLATION`.

## Current Scope and Limitations

- A new subscription takes the best route by the draft's path-selection rule.
  Failover, load balancing, and moving an existing subscription to a better
  route are not implemented.
- Redundant-origin comparison, topology coalescing, and trust-boundary path
  stripping described as optional behavior in the draft are not implemented.
- Hop IDs are opaque routing-loop identifiers, not authentication or
  authorization credentials.
- `HOP_PATH` exposes path length. Protect peer links with TLS and consider this
  topology information when defining trust boundaries.

See the [configuration reference](config.md#relay-architectures) for the full
service and upstream schema.
