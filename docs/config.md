# Configuration Reference

moqx is configured via a YAML file passed with `--config <path>`. This document
describes all configuration options, how they interact, and how to structure
multi-relay deployments.

For the config system internals (how to add fields, the parse/resolve pipeline),
see [dev/config.md](dev/config.md).

## Tooling

```
moqx validate-config --config <path>    # validate without starting
moqx dump-config-schema                 # print JSON schema
moqx --config <path> --strict_config    # reject unknown fields,
                                        # promote warnings to errors
```

## Top-Level Structure

```yaml
relay_id: "my-relay"       # optional; auto-generated if absent
threads: 1                 # optional; IO worker threads, default 1 (see Threading)

cluster:                   # optional; MoQ Cluster negotiation and Hop ID
  { ... }                # used when peering.method is cluster (the default)

peering:                   # optional; which interconnect starts with the process
  method: cluster          # cluster (default) or moq-peering; only one is used
  moq_peering: { ... }     # required when method is moq-peering

listener_defaults:         # optional; QUIC defaults inherited by all listeners
  quic: { ... }
  mvfst: { ... }           # mvfst listeners only

listeners:                 # required; at least one
  main: { ... }

service_defaults:          # optional; defaults inherited by all services
  cache: { ... }
  auth: { max_tokens_per_message: 4 }
  max_subscription_buffer_kb: 51200   # required here or on each service

services:                  # required; at least one
  my-service:
    listeners: [ ... ]     # optional; default: all listeners
    match: [ ... ]
    cache: { ... }
    upstream: { ... }

admin:                     # optional; omit to disable the admin server
  { ... }
```

**Duplicate keys** in any YAML mapping (two map-form listeners or two services
with the same name, a field written twice) fail the load.

---

## Threading

| Field | Default | Notes |
|---|---|---|
| `threads` | 1 | IO worker threads. Must be ≥ 1; more than 1 requires `use_relay_thread: true`. |
| `use_relay_thread` | `true` | One dedicated thread per service holds that service's relay state. |
| `use_local_forwarders` | `false` | Forward to each subscriber from its own IO thread, cutting cross-thread hops on the data path. Requires `use_relay_thread`. |
| `mvfst_bpf_steering` | `true` | BPF reuseport filter that steers each packet to the mvfst worker named in its connection ID. Linux and mvfst only; `false` leaves distribution to the kernel. |

`moqx-run.sh` and the Docker image run 4 threads, with local forwarders on and
BPF steering off; see [tuning.md](tuning.md#relay-config).

---

## Listeners

Each listener binds a port and accepts connections: MOQT over native QUIC and
over HTTP/3 WebTransport, or, with `quic_stack: proxygen_qmux`, MOQT over QMux
on TCP at the `udp.socket` address. HTTP/2 WebTransport is planned
([#152](https://github.com/openmoq/moqx/issues/152)).

`listeners` is a map keyed by listener name:

```yaml
listeners:
  main:
    udp:
      socket:
        address: "::"
        port: 4433
    tls:
      cert_file: /etc/moqx/cert.pem
      key_file:  /etc/moqx/key.pem
    endpoint: /moq-relay
    quic_stack: mvfst         # optional; mvfst (default), picoquic or proxygen_qmux
    moqt_versions: []         # optional; empty = default [16]
    quic: { ... }             # optional; overrides listener_defaults.quic
    mvfst: { ... }            # optional; overrides listener_defaults.mvfst
```

**TLS:** For development only, `tls: {insecure: true}` skips certificate
verification. This is incompatible with `quic_stack: picoquic`.

**moqt_versions:**: Supports 16 and 18; any other value is rejected.

**Duplicate listeners** (same address+port combination) are rejected.

**WebTransport idle timeout (mvfst):** `mvfst.h3_txn_timeout_ms` (per listener
or in `listener_defaults`) closes a WebTransport session after that many ms
without new streams, stream data or datagrams. 0 (default) disables it; the
maximum is a year.

### QUIC Settings

Set under `listener_defaults.quic` (global defaults) or `listeners.<name>.quic`
(per-listener override).

| Field | Default | Notes |
|---|---|---|
| `max_data` | 67108864 (64 MB) | Connection-level flow control window. Must be ≥ `max_stream_data`. |
| `max_stream_data` | 16777216 (16 MB) | Per-stream flow control window. |
| `max_uni_streams` | 8192 | Max concurrent unidirectional streams. Warning if < 100. |
| `max_bidi_streams` | 16 | Max concurrent bidirectional streams. Warning if < 16. |
| `idle_timeout_ms` | 30000 | QUIC idle timeout in ms. Warning if < 5000. |
| `max_ack_delay_us` | 25000 | Max ACK delay (μs). mvfst ignores this (logs at DBG1). |
| `min_ack_delay_us` | 1000 | Min ACK delay (μs). Must be ≤ `max_ack_delay_us`. |
| `default_stream_priority` | 2 | Default stream priority. |
| `default_datagram_priority` | 1 | Default datagram priority. |
| `cc_algo` | `bbr` | Congestion control algorithm (see below). |

**Congestion control algorithms by stack:**
- `mvfst`: `bbr`, `bbr2`, `bbr2modular`, `copa`, `cubic`, `newreno`
- `picoquic`: `bbr`, `bbr1`, `c4`, `cubic`, `dcubic`, `fast`, `newreno`,
`prague`, `reno`

### mvfst Settings

Set under `listener_defaults.mvfst` or `listeners[n].mvfst`. Only mvfst
listeners use them; picoquic listeners ignore the block.

| Field | Default | Notes |
|---|---|---|
| `udp_socket_buffer_bytes` | 0 | UDP socket send and receive buffer. 0 = moxygen's 1 MB. The kernel caps it at `net.core.wmem_max`/`rmem_max`; see [tuning.md](tuning.md#socket-buffers-the-relay-asks-for). |
| `enable_gso` | `true` | Send with GSO; falls back to `sendmmsg` if unavailable. Disable only where GSO is advertised but broken. |
| `max_conn_packets_sent_per_loop` | 48 | Packets written per connection per event-loop pass. |
| `use_recvmmsg` | `true` | Read several packets per syscall. |
| `max_server_recv_packets_per_loop` | 64 | Incoming packets handled per read callback; 0 = unlimited. |
| `num_gro_buffers` | 1 | UDP GRO receive buffers; 1 = off, up to 64. |
| `ignore_path_mtu` | `false` | Send full-size packets without path MTU discovery. Only for networks with a known MTU. |
| `adaptive_loss_time_threshold` | `false` | After a spurious loss, raise the loss time threshold past that packet's lateness. Helps on paths with delay jitter. |
| `max_cwnd_in_mss` | 860000 | Congestion window cap, in MSS. |
| `pacing_enabled` | `true` | Required for `bbr` and `bbr2`. |

Congestion control tunables nest under `bbr`, `bbr2`, `cubic`, `copa` and `l4s`;
`moqx dump-config-schema` lists them with descriptions.

---

## Services

Services define how incoming connections are routed. Each service has one or
more match rules, an optional list of listeners it is available on, optional
cache settings, and an optional upstream.

```yaml
services:
  live:
    listeners: [public, internal]   # optional; default: all listeners
    match:
      - authority: {exact: "live.example.com"}
        path:      {prefix: "/"}
    cache:
      enabled: true
      max_tracks: 1000
      max_groups_per_track: 10
    upstream:
      url: moqt://origin.example.com:4433/moq-relay
      tls: {insecure: false}
    max_subscription_buffer_kb: 51200
```

`max_subscription_buffer_kb` is required, set on the service or in
`service_defaults`. It limits the maximum number of KB the relay holds for one
subscription across all of its streams. When a write would exceed it, the relay
ends the subscription with PUBLISH_DONE TOO_FAR_BEHIND. 0 removes the limit.
The limit applies to client sessions and to this service's upstream sessions.
FETCH responses do not count toward it.

What counts as held depends on the QUIC stack:

- `mvfst` (including every upstream session): bytes written to the transport
  that the peer has not yet acknowledged. In-flight bytes count, so the limit
  must cover the subscription's bitrate times the downstream RTT plus its
  largest object. For example, a 20 Mbps stream holds about 1.25 MB per 500 ms
  of RTT, and one keyframe can add 5 MB.
- `picoquic`: bytes that picoquic has not yet put into a packet, which means
  bytes waiting on flow control, congestion control, or pacing. Sent bytes
  stop counting before the peer acknowledges them.

The shipped configs use 51200 (50 MB). The limit is a last resort against a
subscriber that falls far behind, so it should not trip for a subscriber
that keeps up.

### Matching Rules

Each service has a `match` list. A connection matches the first service whose
authority and path rules both match.

**Authority matchers (highest to lowest precedence):**
1. `{exact: "live.example.com"}` — exact hostname
2. `{wildcard: "*.example.com"}` — single-label subdomains only;
`a.example.com` matches but `a.b.example.com` and `example.com` do not
3. `{any: true}` — catch-all

**Path matchers (within the matched authority tier):**
1. `{exact: "/moq-relay"}` — exact path match
2. `{prefix: "/live/"}` — longest prefix wins; `{prefix: "/"}` matches
any path

Duplicate (authority, path) combinations within a service are rejected.
Across services, they are rejected when the services share a listener.

**picoquic limitation:** Only exact path matches work reliably with
picoquic. Prefix path rules on a service bound to a picoquic listener generate
a warning, and connections may fail to route correctly.

### Listener Binding

`listeners` names the listeners a service is available on. Omit it to make the
service available on every listener.

- Matching runs per listener, over only the services bound to it. A service
  that is not bound to a listener does not exist there: the next-best bound
  match wins, and a session with no bound match is rejected.
- A service bound to several listeners is one service: a publisher on one
  listener feeds subscribers on another.
- An unknown listener name, or one listed twice, is a config error.
- `listeners: []` is a warning: no incoming session can select the service.
  Its upstreams still connect.
- A listener that no service is bound to is a warning: it rejects every session.

```yaml
listeners:
  public: { ... }
  internal: { ... }
services:
  live:
    listeners: [public, internal]
    match:
      - authority: {any: true}
        path: {exact: "/moq-relay"}
  ops:
    listeners: [internal]
    match:
      - authority: {any: true}
        path: {exact: "/ops"}
```

An mvfst listener accepts WebTransport sessions only on its `endpoint` path, so
its WebTransport clients reach only the services that match that path.

`/config` reports each service's resolved `listeners`. `/state` reports the
listener each downstream peer connected on.

---

## Authentication and Authorization

Authentication is configured per service under `services.<name>.auth`. When
enabled, moqx expects MOQT `AUTHORIZATION_TOKEN` parameters whose token type
matches the configured `token_type`. The token value is a CAT/CWT token signed
with an HMAC key shared by the token issuer and the relay.

```yaml
services:
  live:
    match:
      - authority: {exact: "live.example.com"}
        path: {exact: "/moq-relay"}
    auth:
      enabled: true
      token_type: 16
      hmac_keys:
        - id: "cat-dev"
          secret: "replace-with-long-random-secret"
      require_setup_token: true
      allow_request_token_override: true
      strict_claims: false
      max_tokens_per_message: 4
      anonymous_claim:
        - actions: [subscribe, fetch]
          namespace_match: {prefix: ["live"]}
```

| Field | Default | Notes |
|---|---|---|
| `enabled` | `false` | Enables CAT-style authorization for this service. When `false`, nothing is enforced and the other `auth` fields have no effect. |
| `token_type` | `0` | MOQT `AUTHORIZATION_TOKEN` type to accept. Use `16` with CAT4MOQ tokens produced for moqxr's CAT wrapper. Type `0` is valid for private or out-of-band deployments. The value must fit in a QUIC variable integer. |
| `hmac_keys` | empty | Required when `enabled: true`. Each key needs a non-empty `id` and `secret`; duplicate key IDs are rejected. The token issuer must use the same key ID and secret. |
| `require_setup_token` | `true` | Requires a valid setup token authorizing `client_setup` during session setup. If `false`, clients can connect without setup grants; per-request actions still need an authorized token or a matching `anonymous_claim` entry. |
| `allow_request_token_override` | `true` | Lets a token on a request count as one more candidate grant for that request, alongside the session's setup-token grants and `anonymous_claim`. If `false`, request tokens are ignored and authorization uses only the setup token grants and `anonymous_claim`. |
| `strict_claims` | `false` | When `true`, rejects a token that carries any claim other than `exp` and `moqt`, the claims the relay enforces. It also rejects a repeated claim and an `exp` that is not an unsigned integer. The relay does not enforce `moqt-reval`, so a token that carries it is rejected. Keep this `false` for current CAT4MOQ interop unless every issuer is known to send only supported claims. |
| `max_tokens_per_message` | none | Required when `enabled: true`, directly or via `service_defaults.auth.max_tokens_per_message`. Caps how many `AUTHORIZATION_TOKEN` params of the configured `token_type` a single SETUP or request message may carry; a message over the cap is rejected outright, not truncated. Params of other token types are not counted. Bounds per-message verification cost against a client attaching many tokens to one message. Must be >= 1. |
| `anonymous_claim` | empty | Statically-granted scopes applied to every request on this service, regardless of what token (if any) authenticated it. See [Anonymous Claim](#anonymous-claim) below. |

The relay only verifies tokens; it does not call an external grant handler.
Grant decisions are encoded by the token issuer as CAT4MOQ actions and scopes.

A message (SETUP or any per-request message) may carry more than one
`AUTHORIZATION_TOKEN` of the configured `token_type`, up to
`max_tokens_per_message`; a message carrying more than that is rejected
outright. Within the cap, a request is authorized if
any one of the tokens present verifies and covers the action; a token that
fails to verify is simply not counted as a candidate rather than failing the
request outright, as long as some other token, the session's setup grants, or
`anonymous_claim` covers it.

### Anonymous Claim

`anonymous_claim` grants a floor of access to every request on the service,
whether or not the request carries a token at all, whether or not a setup
token was presented, and regardless of what any presented token grants. It
never authorizes `client_setup`/`server_setup` — connecting still needs a
valid setup token whenever `require_setup_token: true`; the claim only ever
widens what an already-connected session (or an anonymous one, when
`require_setup_token: false`) can do per request.

```yaml
auth:
  enabled: true
  hmac_keys: [{id: "cat-dev", secret: "replace-with-long-random-secret"}]
  require_setup_token: true
  max_tokens_per_message: 4
  anonymous_claim:
    - actions: [subscribe, fetch]
      namespace_match: {prefix: ["live"]}
      track_match: {exact: "video"}
```

Each entry:
- `actions` — one or more action names from the table below (aliases and
  numeric IDs accepted, same as `moqx-issuer`'s `--auth-actions`).
  `client_setup`/`server_setup` are rejected.
- `namespace_match` — optional; a list of namespace segments matched by
  `exact`, `prefix`, `suffix`, or `contains`. Omit to match any namespace.
- `track_match` — optional; a single track name matched by `exact`, `prefix`,
  `suffix`, or `contains`. Omit to match any track.

The four match modes are the same `BinaryMatchType` (exact/prefix/suffix/contains)
CAT4MOQ tokens use for their own namespace/track claims, so an `anonymous_claim`
scope matches namespaces and tracks with identical semantics to a token-granted
scope.

### Issuing Tokens

Use the standalone `moqx-issuer` binary as the deployment/operator tool. For
production-like deployments, point it at the same config file used by the relay
so the selected service key is read from one place:

```bash
moqx-issuer \
  --config /etc/moqx/config.yaml \
  --auth-service live \
  --auth-key-id cat-dev \
  --auth-actions client_setup,publish_namespace,publish \
  --auth-namespace live/event/main \
  --auth-track video \
  --auth-ttl-seconds 3600
```

The default output is `base64:<token>`, which is suitable for tools that accept
a base64-prefixed CAT token string. Use `--auth-output base64`, `hex`, or `raw`
when integrating with software that expects a different representation.

For local development, the command can issue from an explicit secret without
reading relay config:

```bash
moqx-issuer \
  --auth-key-id cat-dev \
  --auth-secret replace-with-long-random-secret \
  --auth-actions client_setup,publish_namespace,publish \
  --auth-namespace live/event/main \
  --auth-track video
```

Supported action names are:

| Name | Draft action |
|---|---|
| `client_setup` or `setup` | `CLIENT_SETUP` |
| `server_setup` | `SERVER_SETUP` |
| `publish_namespace` or `announce` | `ANNOUNCE` |
| `subscribe_namespace` | `SUBSCRIBE_NAMESPACE` |
| `subscribe` | `SUBSCRIBE` |
| `request_update` or `subscribe_update` | `SUBSCRIBE_UPDATE` |
| `publish` | `PUBLISH` |
| `fetch` | `FETCH` |
| `track_status` | `TRACK_STATUS` |

The CLI also accepts the numeric draft action values `0` through `8`.
`--auth-actions` defaults to `client_setup,publish_namespace,publish`, which is
the common publisher case. The `client_setup` grant is unconstrained. Namespace
actions are scoped by `--auth-namespace`, and track actions are scoped by
`--auth-namespace` plus `--auth-track` when a track is provided. Namespace
segments are slash-separated on the CLI, for example `live/event/main`.

### Using Auth From moqxr

A publisher app should obtain a token before connecting or before issuing a
request that needs narrower grants, then pass that token as the MOQT
`AUTHORIZATION_TOKEN` value with the relay's configured token type. For moqxr's
auth example, the easiest local setup is to call the moqx-issuer command:

```bash
export CATAPULT_CAT4MOQ_COMMAND='moqx-issuer \
  --config /etc/moqx/config.yaml \
  --auth-service live \
  --auth-key-id cat-dev \
  --auth-actions client_setup,publish_namespace,publish \
  --auth-namespace {namespace} \
  --auth-track {track}'
```

The placeholders are replaced by the publisher example. If a deployment issues
one broad publisher token, keep the fixed action list shown above. If it issues
request-specific tokens, include the `{action}` placeholder and map it to the
grant set required by that request; for the current moqxr auth example, the
publisher command is invoked with `{action}` set to `client_setup` for the setup
token and `publish` for the action token.

### Docker Usage

The container entrypoint routes an `issue-cat-token` argument to the bundled
`moqx-issuer` binary, so operators can generate tokens from the same image used
to run the relay:

```bash
docker run --rm \
  -v "$PWD/moqx-auth.yaml:/etc/moqx/config.yaml:ro" \
  moqx:tag issue-cat-token \
  --config /etc/moqx/config.yaml \
  --auth-service live \
  --auth-key-id cat-dev \
  --auth-actions client_setup,publish_namespace,publish \
  --auth-namespace live/event/main \
  --auth-track video
```

### Operational Notes

- Keep HMAC secrets out of source control and local shell history.
- Rotate keys by adding a new `hmac_keys` entry, issuing new tokens with that
key ID, waiting for old tokens to expire, then removing the old key.
- A token signed with an unknown key ID, wrong secret, expired grant, wrong
namespace, or wrong track is rejected.
- Auth is currently service-local. Upstream relay connections still have no
application-level credential exchange beyond TLS.
- `max_tokens_per_message` is inheritable from `service_defaults.auth`

---

## Cache

Cache settings can be specified as global defaults under
`service_defaults.cache` and overridden per service under
`services.<name>.cache`. Overrides are applied field-by-field; any field not set
at the service level inherits from `service_defaults`.

| Field | Default | Notes |
|---|---|---|
| `enabled` | — | Required (after merge). Enable or disable the cache for this service. |
| `max_tracks` | — | Required (after merge). Maximum number of tracks held in cache. |
| `max_groups_per_track` | — | Required (after merge). Must be ≥ 1. Max cached groups per track. |
| `max_cached_mb` | 16 | Total cache size limit in MB across all tracks. Must not be 0. |
| `min_eviction_kb` | 64 | Eviction batch floor in KB. When the cache exceeds `max_cached_mb`, LRU tracks are evicted until usage falls to `max_cached_mb − min_eviction_kb`. |
| `max_cache_duration_s` | 86400 (1 day) | Hard cap on how long any track can be cached. Must not be 0. Publisher-set durations are clamped to this value. Also used as the default track duration when `default_max_cache_duration_s` is absent. |
| `default_max_cache_duration_s` | absent | Default TTL for tracks that don't carry a publisher-set duration. **absent** → use `max_cache_duration_s`. **0** → do not cache unless the publisher explicitly sets a duration (opt-in caching). **N** → cache for N seconds by default. |

---

## Upstreams

An upstream connects this relay to another relay in the network. Despite the
name, the connection is fully bidirectional: publishers and subscribers can be
on either side. When a client subscribes to a track that has no local publisher,
moqx forwards the subscription upstream; when a client publishes a track, that
track becomes available to subscribers on any relay in the connected network.

moqx maintains the upstream connection at all times, reconnecting automatically
with exponential backoff up to 60 seconds on failure.

The upstream connection uses MoQT draft 18, so the upstream relay's listener must
include 18 in [`moqt_versions`](#listeners).

```yaml
services:
  live:
    match:
      - authority: {any: true}
        path:      {prefix: "/"}
    upstream:
      url: moqt://hub.example.com:4433/moq-relay
      tls:
        insecure: false      # set true only for development
        # ca_cert: /etc/moqx/ca.pem  # optional custom CA;
                                     # mutually exclusive with insecure: true
      connect_timeout_ms: 5000       # optional; default 5000
      idle_timeout_ms: 5000          # optional; default 5000
```

To peer with several relays, list them under `upstreams` instead. Each entry
takes the same fields as `upstream`, and a service cannot set both:

```yaml
    upstreams:
      - url: moqt://hub-a.example.com:4433/moq-relay
        relay_cost: 0                # optional; default 1
      - url: moqt://hub-b.example.com:4433/moq-relay
```

**`relay_cost`** is the MoQ Cluster `RELAY_COST` that this relay declares on
the link. It prices what this relay sends to that upstream: 0 makes the link
free, and the default 1 ranks routes by hop count. It is only sent when
[`cluster`](#cluster) is enabled.

**`tls.insecure: true`** skips certificate verification. It cannot be
combined with `ca_cert`.

**`ca_cert`** pins a custom CA certificate, a PEM file. The upstream's
certificate must chain to it and match the URL's host. The relay does not
start if the file cannot be loaded. Omit to use the system trust store.

### Relay Architectures

Together, a set of connected moqx relays acts as a single logical MOQT relay:
publishers and subscribers can connect to any node, and track routing is
transparent across the network.

Upstream relationships are configured per-service, not per-server. Because track
namespaces and caches cannot be shared across services, each service forms its
own independent upstream graph. Two services on the same physical server can
peer with completely different upstream servers.

Tree topologies remain the simplest deployment model. A service may also contain
a relay cycle when every relay in that cycle negotiates MoQ Cluster by declaring
the `HOP_ID` setup option. moqx assigns one random Hop ID to the running relay
context, retains the origin-to-relay path on namespace advertisements, appends
its own ID when forwarding, and drops an advertisement if its own ID is already
present. It does not advertise a path to a peer whose Hop ID is on that path.

```
          relay-a ─┐
          relay-b ──── relay-root (no upstream)
relay-c ──── relay-d ─┘
relay-e ─┘
```

Publishers and subscribers can connect to any relay. Subscriptions are forwarded
up toward the root as needed, and data flows back down to wherever subscribers
are.

MoQ Cluster negotiation is automatic between supporting moqx peers. The optional
[`cluster`](#cluster) block can disable it or set the Hop ID. An advertisement
from a non-negotiated session gets the anonymous path `[0]`. When it next
crosses a negotiated relay link, the outgoing path is `[0, local-relay]`. A
non-negotiated subscriber receives NAMESPACE without HOP_PATH or ROUTE_COST.

Loop prevention is only end-to-end across links that negotiated MoQ Cluster.
Keep any graph segment containing a relay without MoQ Cluster acyclic, because
that relay cannot preserve the path needed to recognize a returning
advertisement.

### Peering method

A process starts one interconnect. `peering.method` selects it:

| Method | What starts |
|---|---|
| `cluster` | MoQ Cluster on draft 18+ links. This is the default, including when `peering` is omitted. |
| `moq-peering` | A [moq-peering](https://github.com/Quicr/moq-peering) node. MoQ Cluster is not negotiated. |

Setting both an enabled `cluster` block and `peering.method: moq-peering` is rejected. With `moq-peering`, omit `cluster` or set `cluster.enabled: false`. A `peering.moq_peering` block is rejected when the method is `cluster`.

`moq-peering` binds a UDP port and dials the configured peers when the process starts. The settings follow `moq::peering::Config`:

```yaml
peering:
  method: moq-peering
  moq_peering:
    node_id: edge-sea-1       # optional; defaults to relay_id. Hashed as the node id.
    node_type: edge           # edge (default), via, or stub
    node_metric: 100          # cost advertised for this node. Lower path sums are preferred.
    listen_port: 33434        # UDP port for inbound peering
    tls:
      cert_file: /etc/moqx/cert.pem
      key_file: /etc/moqx/key.pem
    peers:                    # optional; omit to only accept connections
      - host: 192.0.2.10
        port: 33440
        metric: 150           # optional session metric, used when greater than node_metric
    object_ttl_ms: 5000
    check_interval_ms: 5000   # reconnect interval; minimum 2000
    init_queue_size: 5000     # accepted by the library and not read by the current library
    debug: false
```

MoQ Peering does not make a client request wait for a peer connection or for
its announcements. A request can therefore arrive before a route from a peer
is known. When a peer connects or reconnects, MoQ Peering refreshes the
announcement and subscription information for that session; routes and object
delivery become available as that information is exchanged. This lets the
relay start serving from learned routes without holding requests open for peer
connectivity.

`node_id` is a contact label. The numeric node id is the hash of that string, printed by the admin API as `high.high:low.low`. Give every node a distinct label.

`tls.cert_file` and `tls.key_file` are paths. The key must be an unencrypted PEM private key. moqx checks that the certificate is currently valid, that its key is RSA of at least 2048 bits or EC of at least 256 bits, that the signature is not MD5 or SHA-1, and that the key matches the certificate. A key that is readable by group or others produces a warning; restrict it to the relay user (`0600`). The private key is not written into the admin `/config` dump.

moq-peering does not validate the peer's certificate. Treat the peering network as trusted, or terminate it where the packets are already authenticated. The admin routes that dial a peer belong on the existing admin listener; keep that listener on a trusted address. `add-peer` accepts a hostname or an IP literal, including private addresses, and rejects a URL.

Service `upstream` entries are not dialed when `moq-peering` is selected. The load warns when any service has an upstream.

The admin API can add, remove, and restart a peer after startup, and can read each session. See [Admin Server](#admin-server).

With `moq-peering`, a client's `PUBLISH_NAMESPACE` is announced to peers as a namespace, and a client's `PUBLISH` is announced as a track. Withdrawing either message withdraws the announcement. A peer's namespace announcement is published into every service relay, so a local `SUBSCRIBE_NAMESPACE` receives it. A peer's track announcement is published the same way, so a local `SUBSCRIBE_TRACKS` receives the `PUBLISH`. Names learned from a peer are not announced back.

Objects use that same split. When a peer subscribes to a locally published track, each object the publisher writes is sent on the peering session with the same MoQT framing a client session uses. A new subgroup stream starts with the subgroup header, and each following object on that stream is a MoQT object. A datagram is a MoQT datagram. When a local subscriber wants a track a peer announced, moqx subscribes on the peering session, parses those bytes with the client stream parser, and writes them into that publish, so the local subscriber gets them. Objects that arrived from a peer are not sent back out. The payload cap is 1MiB. `FETCH` stays on MoQT sessions.

### Cluster

Used when `peering.method` is `cluster` (the default).

```yaml
cluster:
  enabled: true   # optional; default true
  hop_id: 1001    # optional; random if absent, 0 is anonymous
```

With `enabled: false`, listeners and upstream connections do not declare
`HOP_ID`, so no link negotiates MoQ Cluster. This lets the relay peer with a
relay or relay network that does not support MoQ Cluster, or that implements an
incompatible version of it. A negotiated link closes the session on a malformed
`HOP_PATH`, so an incompatible peer otherwise cannot stay connected.

Disabling MoQ Cluster leaves loop prevention to the operator:

- Neither this relay nor its peers can detect an advertisement that returns
  through it. Keep the part of the graph that contains this relay a tree. Relays
  peer in both directions, so two paths between any pair of relays form a
  cycle.
- Peers see this relay's advertisements as anonymous, without its path or
  ROUTE_COST, so they cannot rank routes through it.

The Hop ID applies to every service, and each relay needs its own. Two relays
with the same `hop_id` drop each other's advertisements as loops, so they cannot
peer. A relay with Hop ID 0 withholds its identity and cannot detect loops
through itself. See [moq-cluster.md](moq-cluster.md).

### Limitations

- **Routes are chosen once.** When several upstreams advertise the same
namespace over MoQ Cluster, a new subscription takes the best route: one
without an anonymous hop, then the lowest cost, then the shortest path. An
existing subscription stays on its route, and failover and load balancing
across upstreams are not currently supported. A link that did not negotiate MoQ
Cluster still replaces any earlier such advertisement of the namespace.
- **Cycles need MoQ Cluster on every relay.** Negotiated moqx peers detect
returning namespace advertisements, but a relay without MoQ Cluster cannot carry
the hop path through the graph.
- **No upstream authentication.** Upstreams are connected without any
application-level credential exchange beyond TLS.
- **Upstreams are chosen by namespace, not per track.** A track routes to the
upstream session that advertised its namespace. The configuration does not
route individual tracks.

---

## Logging

### `logging.mlog`

Enables MoQ-level (mlog) structured logging of control messages and protocol
events, one file per session.

```yaml
logging:
  mlog:
    dir: "/var/log/moqx/mlog"   # required to enable; empty string disables
    sample_rate: 0.01            # log 1% of sessions (default: 0.0 = none)
```

| Field | Type | Default | Description |
|---|---|---|---|
| `dir` | `string` | — (disabled) | Output directory. Each session writes to `<dir>/<dcid>.mlog`. If empty, mlog is disabled. |
| `sample_rate` | `float` | `0.0` | Fraction of sessions to log, in `[0.0, 1.0]`. `0.0` logs none; `1.0` logs all sessions; `0.01` logs ~1%. |


#### Lifecycle

| Field | Lifecycle |
|---|---|
| `dir`, `sample_rate` | Static — requires restart |

---

## Admin Server

The admin server exposes an HTTP management API. It is optional; omit the
`admin` section to disable it.

```yaml
admin:
  address: "::1"
  port: 8080
  plaintext: true      # OR tls (mutually exclusive)
  # tls:
  #   cert_file: /etc/moqx/admin-cert.pem
  #   key_file:  /etc/moqx/admin-key.pem
  #   alpn: [h2, "http/1.1"]
  # track_metrics_enabled: true                 # per-track counting and /metrics/track
  # track_metrics_endpoint_default_limit: 10    # default tracks per /metrics/track scrape
  # track_metrics_endpoint_max_limit: 1000      # ceiling on the ?limit= parameter
```

Either `plaintext: true` or a `tls` block must be set, but not both.

`track_metrics_enabled` defaults to `false`, which leaves the counting filters
out of the data path entirely — nothing is installed, so there is no per-object
cost — and `/metrics/track` answers `503` rather than an empty scrape that would
read as "no live tracks".

`track_metrics_endpoint_default_limit` and `track_metrics_endpoint_max_limit`
bound `/metrics/track`; the default must not exceed the max. The limit is a
guard rail, not a selection rule: a query matching more tracks than the limit is
rejected rather than truncated, because an arbitrary subset would give
Prometheus a series set that reshuffles between scrapes.  See [docs/metrics.md] (metrics.md).

### Endpoints

| Method | Path | Description |
|---|---|---|
| `GET` | `/info` | Returns `{"service":"moqx","version":"...","start_time":<epoch seconds>,"uptime_seconds":<n>}`. |
| `GET` | `/metrics` | Prometheus-format metrics. See [docs/metrics.md](metrics.md) (pending PR #137). |
| `GET` | `/metrics/track` | Per-track Prometheus metrics for live tracks. See [docs/metrics.md](metrics.md). |
| `GET` | `/state` | Relay state: active subscriptions, namespace tree, cache stats, and peering status. The top-level `peering` object identifies the selected method; for `cluster`, service state includes downstream peers and upstream links, and for `moq-peering`, `peering` includes the node and peer session details also returned by `/peering`. Always `Transfer-Encoding: chunked`, streamed as the walk produces it. Per-subscription counters are ingress-side (`total_groups_received`, `total_objects_received`, `largest`), counted on the relay executor; for subscriber counts use `moqx_track_subscribers` from `/metrics/track`. |
| `GET` | `/peering` | The peering method this process started. `cluster` returns `enabled` and, when MoQ Cluster is on, `hop_id`. `moq-peering` returns the local node id and each session from `PeerManager::Peers`: origin, link status, smoothed RTT, session and node metrics, subscribe and publish counts, and open streams. |
| `POST` | `/peering/add-peer` | Dial a peer, or remember the address when that peer is already accepted. Body: `{"host":"...","port":33434,"metric":150}`. `metric` is optional. |
| `POST` | `/peering/remove-peer` | Disconnect `{"host":"...","port":33434}` and drop its forwarding state. |
| `POST` | `/peering/restart-peer` | Drop the connection and its forwarding state. A dialed peer is connected again. Body: `{"host":"...","port":33434}`. |
| `POST` | `/peering/set-peer-metric` | Set the metric advertised on one session when it is greater than the node metric. Body: `{"host":"...","port":33434,"metric":200}`. |

A peering POST that names the method which is not running answers `409`. A duplicate or missing peer answers `409` with the moq-peering result (`ok`, `action`, `message`). A host that is not a hostname or IP literal answers `400`. The body is at most 4096 bytes.

---

## Reloading

Config is read once at startup; every change requires a restart. Reload is
planned ([#786](https://github.com/openmoq/moqx/issues/786),
[design/hot-reloading.md](/design/hot-reloading.md)).
On `SIGHUP` the relay logs a warning and keeps running.
