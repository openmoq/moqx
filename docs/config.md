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
threads: 1                 # optional; default 1 (only 1 currently supported)

listener_defaults:         # optional; QUIC defaults inherited by all listeners
  quic: { ... }

listeners:                 # required; at least one
  - { ... }

service_defaults:          # optional; cache defaults inherited by all services
  cache: { ... }

services:                  # required; at least one
  my-service:
    match: [ ... ]
    cache: { ... }
    upstream: { ... }

admin:                     # optional; omit to disable the admin server
  { ... }
```

---

## Listeners

Each listener binds a UDP port and accepts MoQ-over-WebTransport connections.

```yaml
listeners:
  - name: main
    udp:
      socket:
        address: "::"
        port: 4433
    tls:
      cert_file: /etc/moqx/cert.pem
      key_file:  /etc/moqx/key.pem
    endpoint: /moq-relay
    quic_stack: mvfst         # optional; default mvfst
    moqt_versions: []         # optional; empty = all supported versions
    quic: { ... }             # optional; overrides listener_defaults.quic
```

**TLS:** For development only, `tls: {insecure: true}` skips certificate
verification. This is incompatible with `quic_stack: picoquic`.

**Duplicate listeners** (same address+port combination) are rejected.

### QUIC Settings

Set under `listener_defaults.quic` (global defaults) or `listeners[n].quic`
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

---

## Services

Services define how incoming connections are routed. Each service has one or
more match rules, optional cache settings, and an optional upstream.

```yaml
services:
  live:
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
```

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

Duplicate (authority, path) combinations across all services are rejected.

**picoquic limitation:** Only exact path matches work reliably with
picoquic. Prefix path rules will generate a warning and connections may fail to
route correctly.

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

> **Note:** `max_cached_mb`, `min_eviction_kb`, `max_cache_duration_s`,
> and `default_max_cache_duration_s` are pending in PR #140 and not yet
> in the released config.

---

## Upstreams

An upstream connects this relay to another relay in the network. Despite the
name, the connection is fully bidirectional: publishers and subscribers can be
on either side. When a client subscribes to a track that has no local publisher,
moqx forwards the subscription upstream; when a client publishes a track, that
track becomes available to subscribers on any relay in the connected network.

moqx maintains the upstream connection at all times, reconnecting automatically
with exponential backoff up to 60 seconds on failure.

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

**`tls.insecure: true`** skips certificate verification. It cannot be
combined with `ca_cert`.

**`ca_cert`** pins a custom CA certificate. Omit to use the system trust
store.

### Relay Architectures

Together, a set of connected moqx relays acts as a single logical MOQT relay:
publishers and subscribers can connect to any node, and track routing is
transparent across the network.

Upstream relationships are configured per-service, not per-server. Because track
namespaces and caches cannot be shared across services, each service forms its
own independent upstream tree. Two services on the same physical server can peer
with completely different upstream servers.

Any tree topology is valid. The only requirement is that within each service's
upstream graph, exactly one node has no upstream configured — that is the root.
All other nodes connect to their upstream, which may itself connect further up
the tree, forming arbitrarily deep tiers.

```
          relay-a ─┐
          relay-b ──── relay-root (no upstream)
relay-c ──── relay-d ─┘
relay-e ─┘
```

Publishers and subscribers can connect to any relay. Subscriptions are forwarded
up toward the root as needed, and data flows back down to wherever subscribers
are.

The constraint is simply that the upstream graph for each service must be
acyclic. **No loops are permitted.** A cycle (A → B → C → A) causes subscription
forwarding to loop indefinitely. moqx does not detect loops at runtime — it is
the operator's responsibility to ensure the topology is loop-free.

### Limitations

- **Single upstream per service.** Each service can have at most one
upstream. Multiple upstreams, failover, or load balancing across upstreams are
not currently supported.
- **No loop detection.** Operators must manually ensure the relay topology
is acyclic.
- **No upstream authentication.** Upstreams are connected without any
application-level credential exchange beyond TLS.
- **Upstream connection is per-service, not per-track.** A single upstream
session is shared for all tracks within a service; there is no per-track
upstream routing.

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
```

Either `plaintext: true` or a `tls` block must be set, but not both.

### Endpoints

| Method | Path | Description |
|---|---|---|
| `GET` | `/info` | Returns `{"service":"moqx","version":"..."}`. |
| `GET` | `/metrics` | Prometheus-format metrics. See [docs/metrics.md](metrics.md) (pending PR #137). |
| `GET` | `/state` | Relay state: connected peers, active subscriptions, namespace tree, and cache stats. Pending PR #146. |

---

## Configuration Lifecycle

Some settings take effect immediately on reload; others require new connections
or a restart.

| Lifecycle | When changes apply | Examples |
|---|---|---|
| **Static** | Process restart required | listeners, admin, relay_id, TLS certificates, QUIC stack |
| **Reload:NewConn** | New connections/sessions only | service match rules, upstream URL |
| **Reload:NewSub** | New subscriptions/tracks only | cache settings |
| **Reload:Immediate** | All connections immediately | (reserved for future fields) |

Send `SIGHUP` to reload the config file. Static fields are ignored on reload; a
warning is logged if they differ from the running config.
