# Deploying MoQ Cluster

moqx implements
[`draft-lcurley-moq-cluster-01`](ref/draft-lcurley-moq-cluster-01.txt) to run
a mesh of relays. Each namespace advertisement carries the path of Hop IDs it
crossed, starting at its publisher, and the accumulated cost of that path. A
relay uses the path to drop advertisements that loop back to it, and the cost
to choose between several paths to one namespace. The draft is the normative
reference. This guide describes how moqx deploys and behaves.

## Deployment Requirements

Every moqx binary in a mesh must contain both parts of the implementation:

- the moqx relay policy and upstream behavior;
- the pinned moxygen wire-format and per-session negotiation support.

The `deps/moxygen` submodule records the required revision. Build and runtime
instructions remain in [`BUILD.md`](../BUILD.md) and [`RUNNING.md`](../RUNNING.md).
Do not introduce a cycle using a moqx image paired with an older moxygen
artifact.

Relay-to-relay links use MOQT draft 18, and MoQ Cluster negotiates only on draft
18 or later. Every relay that accepts upstream connections must include 18 in its
listener `moqt_versions`.

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

Each upstream link takes a `relay_cost`, which this relay declares as
`RELAY_COST` on that link. It prices what this relay sends to that peer, so the
peer adds it to the cost of every path learned from this relay. It does not
change which path this relay picks. Only the relay that dials a link declares a
cost on it, so the listening side's traffic over that link always costs 1. The
default 1 ranks paths by hop count, and 0 makes the link free.

## Protocol Behavior

Negotiation is session-local: the extension is active on a session only when
both ends declare a Hop ID in the `HOP_ID` setup option. On such a session:

- namespace advertisements carry `HOP_PATH` and `ROUTE_COST`. A relay appends
  its own Hop ID and adds the sender's link cost before forwarding;
- an advertisement whose path already contains this relay's Hop ID is dropped
  before it is registered or forwarded;
- an advertisement with a malformed or missing `HOP_PATH` closes the session
  with `PROTOCOL_VIOLATION`.

A namespace keeps one route per advertising session. Routes from MoQ Cluster
sessions coexist. A route from a session that did not negotiate the extension
gets the anonymous path `[0]` and replaces any earlier such route, as before.
Routes rank by the draft's path-selection rule: a path without 0 first, then
the lowest cost, then the shortest path, then the newest. Only the most
specific namespace that has routes is consulted.

Advertisement to each namespace subscriber:

- each subscriber is told the best route it may use. A path that contains the
  subscriber's Hop ID is never advertised to it;
- when that route's path or cost changes, the subscriber gets the repeated
  NAMESPACE as an update. When its publisher changes, the namespace is
  withdrawn and advertised again;
- NAMESPACE_DONE goes only to a subscriber that was told the namespace and has
  no route left;
- a repeated NAMESPACE, or a REQUEST_UPDATE on a PUBLISH_NAMESPACE, from the
  same origin updates its route in place and leaves its subscriptions running.
  A new origin replaces the route and ends the subscriptions it served.

Request routing:

- a SUBSCRIBE, FETCH, or TRACK_STATUS from a MoQ Cluster session skips routes
  whose path contains that session's Hop ID, and routes from the session
  itself. When only such routes remain, the request gets `DOES_NOT_EXIST`;
- a track keeps one upstream subscription. A MoQ Cluster session that the
  existing subscription's source runs through gets `DOES_NOT_EXIST` instead of
  joining it;
- sessions that did not negotiate the extension route as before, including an
  end user subscribing to a track it publishes.

## Configure a Relay Mesh

A service peers with one `upstream` or with each entry of an `upstreams` list,
reconnecting each link with backoff after a failure. Services do not share
namespace or path state. A cycle points each service at the next relay:

```text
relay-a -> relay-b -> relay-c -> relay-a
```

```yaml
# relay-a
upstream:
  url: "moqt://relay-b.example.com:4433/moq-relay"
  tls:
    insecure: false
```

relay-b and relay-c name relay-c and relay-a the same way. A relay with several
neighbors lists them, pricing each link:

```yaml
# relay-b: edge-1 sees paths through relay-b as free, edge-2 as costing 5
upstreams:
  - url: "moqt://origin.example.com:4433/moq-relay"
  - url: "moqt://edge-1.example.com:4433/moq-relay"
    relay_cost: 0
  - url: "moqt://edge-2.example.com:4433/moq-relay"
    relay_cost: 5
```

The shortened fragments belong under the same `services.<name>` entry on their
respective relays. The complete listener, TLS, service, and upstream schemas are
in [the configuration reference](config.md#relay-architectures).

Use `tls.insecure: true` only in isolated development or integration-test
networks. In production, use certificates trusted by the system trust store or
set `ca_cert` to the cluster CA. `ca_cert` and `insecure: true` are mutually
exclusive.

All links in a cycle must negotiate MoQ Cluster. If any relay or link does not
carry `HOP_PATH`, loop detection is no longer end-to-end. Keep every graph
segment containing a relay without MoQ Cluster acyclic.

## Rolling Upgrade Procedure

Do not add a cycle while unsupported relays remain in that cycle.

1. Keep the current topology acyclic.
2. Deploy the MoQ Cluster-capable moxygen and moqx build to every relay that
   will participate.
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
`upstream.state`, or each entry of `upstreams`, reports whether a link is
connected.

For a source checkout, run the relay integration tests:

```bash
ctest --test-dir build \
  --output-on-failure \
  -R '^(relay_chain|relay_hops_cycle|relay_pyramid|relay_upstreams|relay_cluster_diamond)$'
```

## Current Scope and Limitations

- A new subscription takes the best route. Failover, load balancing, and moving
  an existing subscription to a better route are not implemented.
- Tracks pushed with PUBLISH carry no hop path, so a relay cycle can still loop
  them. Keep PUBLISH-only traffic off cycles.
- Redundant-origin comparison, topology coalescing, and trust-boundary path
  stripping described as optional behavior in the draft are not implemented.
- Hop IDs are opaque routing-loop identifiers, not authentication or
  authorization credentials.
- `HOP_PATH` exposes path length. Protect peer links with TLS and consider this
  topology information when defining trust boundaries.

See the [configuration reference](config.md#relay-architectures) for the full
service and upstream schema.
