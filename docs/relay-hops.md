# Deploying MoQ Cluster

moqx implements [`draft-lcurley-moq-cluster-00`](draft-lcurley-moq-cluster-00.txt).
This replaces the earlier relay-hops extension. The existing guide URL is retained
for links from deployments and test documentation.

The extension carries a publisher-to-relay `HOP_PATH` and accumulated
`ROUTE_COST` on namespace advertisements. Each endpoint declares its Hop ID in
`RELAY_HOPS` during setup. A relay filters both advertisements and request
routing against the receiving peer's identity, preventing a subscription from
returning content that already passed through that peer.

## Build and protocol requirements

Both moqx and its moxygen dependency need cluster support. The dependency revision
is recorded in `cmake/dependencies.cmake`; see [BUILD.md](../BUILD.md) for the
source-build workflow. Do not pair this relay with a pre-cluster dependency.

Cluster negotiation requires draft 18 advertisement stream lifetimes. Configure
mesh listeners explicitly with `moqt_versions: [18]`. The default listener
versions remain `[14, 16]`. Upstream clients offer draft 18 and draft 16; the
latter provides ordinary chaining without the cluster extension. The old
zero-length setup flag and request-level exclusion parameter are not the cluster
wire format.

## Configure peers

Every relay in a cycle must negotiate the extension and use a unique nonzero
Hop ID. Keep any segment containing nonnegotiating relays acyclic.

```yaml
cluster:
  enabled: true
  hop_id: 1001
  cost_grace_ms: 2000

listeners:
  - name: mesh
    udp:
      socket:
        address: "::"
        port: 4433
    endpoint: "/moq-relay"
    moqt_versions: [18]
    tls:
      cert_file: /etc/moqx/server.crt
      key_file: /etc/moqx/server.key

service_defaults:
  cache:
    enabled: true
    max_tracks: 100
    max_groups_per_track: 3

services:
  live:
    match:
      - authority: {any: true}
        path: {exact: "/moq-relay"}
    upstreams:
      - url: "moqt://relay-b.example.com:4433/moq-relay"
        relay_cost: 0
      - url: "moqt://relay-c.example.com:4433/moq-relay"
        relay_cost: 5
```

Use `upstream` for one peer or `upstreams` for several; specifying both on one
service is rejected. Connections and their reconnect backoff are independent.
Routing state belongs to the service. The process Hop ID is shared across its
services and sessions.

Omitting `hop_id` generates a random nonzero uint64 for this process lifetime.
A configured ID survives restarts. `hop_id: 0` explicitly declares anonymity;
it does not identify an endpoint for loop detection or peer filtering.
The existing `relay_id` is an operational name and is independent of Hop ID.

The client sets `relay_cost`; the server does not send it. Both endpoints add
that price when receiving advertisements over the link. An omitted value means
1, while 0 means a free link. Addition saturates at the largest uint64.

## Route and advertisement lifetime

For a namespace, eligible routes are ordered by accumulated cost, then shorter
path, then most recent receipt. A peer receives the best route that excludes
its own nonzero ID, including a standby when the preferred route passes through
that peer. An excluded more-specific namespace does not fall back to a broader
namespace with potentially different content.

Advertisements with the same nonzero first Hop ID represent interchangeable
content. Different first IDs, or origin 0, replace the content identity. A
publisher that does not negotiate the extension is represented by 0; forwarding
its advertisement produces `[0, local-hop-id]`. Multiple zeros are valid, but
repeated nonzero IDs are invalid. Redundant mesh paths require a publisher with
a negotiated nonzero identity; anonymous advertisements do not provide failover
or update continuity. moqx suppresses optional warm/cold price changes for
anonymous origins because each repeat would replace downstream content.
Missing or malformed required paths close a
negotiated session with `PROTOCOL_VIOLATION`.

An update stays on its original `PUBLISH_NAMESPACE` request stream or
`SUBSCRIBE_NAMESPACE` response stream. A cost update does not retract the
namespace. Closing an old advertisement cannot withdraw its replacement.
Healthy existing subscriptions stay attached when same-origin prices change;
seamless reparenting of an established subscription is not implemented.

Cluster-routed subscriptions share an upstream ingest per selected route. Their
forwarders run on the relay executor, including in local-forwarder mode. Fetches
on these routes go to the selected source: the existing cache is keyed only by
track name and cannot distinguish alternative routes safely. Ordinary exact-track
publishes retain their existing forwarding and cache behavior.

## Validation

The unit suite covers setup negotiation, path validation, cost saturation,
anonymous origins, same-stream updates, standby selection, and withdrawal
ownership. The cycle test explicitly selects draft 18:

```bash
ctest --test-dir build/default --output-on-failure \
  -R '^(relay_chain|moq_cluster_chain|relay_hops_cycle|relay_pyramid)$'
```

The cycle test name retains its historical spelling. Verify namespace
propagation and media objects as well as successful session setup when testing a
deployment. A connected session alone does not establish that routing works.
