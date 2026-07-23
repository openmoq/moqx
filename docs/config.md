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

Each listener binds a port and accepts connections.  For now, only UDP+QUIC
is supported, both MOQT using native QUIC and MOQT over HTTP/3 WebTransport.
We will add support for HTTP/2 WebTransport and MOQT over QMux in the
future.

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
    moqt_versions: []         # optional; empty = default [14, 16]
    quic: { ... }             # optional; overrides listener_defaults.quic
```

**TLS:** For development only, `tls: {insecure: true}` skips certificate
verification. This is incompatible with `quic_stack: picoquic`.

**moqt_versions:**: Currently supports 14 and 16.

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

Services define how incoming connections are routed and can match MOQT
sessions arriving on any listener. Each service has one or more match rules,
optional cache settings, and an optional upstream.

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
```

| Field | Default | Notes |
|---|---|---|
| `enabled` | `false` | Enables CAT-style authorization for this service. |
| `token_type` | `0` | MOQT `AUTHORIZATION_TOKEN` type to accept. Use `16` with CAT4MOQ tokens produced for moqxr's CAT wrapper. Type `0` is valid for private or out-of-band deployments. The value must fit in a QUIC variable integer. |
| `hmac_keys` | empty | Required when `enabled: true`. Each key needs a non-empty `id` and `secret`; duplicate key IDs are rejected. The token issuer must use the same key ID and secret. |
| `require_setup_token` | `true` | Requires a valid setup token authorizing `client_setup` during CLIENT_SETUP. If `false`, clients can connect without setup grants, but publish/subscribe requests still need an authorized setup or request token. |
| `allow_request_token_override` | `true` | Allows a token on a request to replace the session setup grants for that request. If `false`, request tokens are ignored and authorization uses only the setup token grants. |
| `strict_claims` | `false` | Rejects unsupported claims when `true`. Keep this `false` for current CAT4MOQ interop unless every issuer is known to send only supported claims. |

The relay only verifies tokens; it does not call an external grant handler.
Grant decisions are encoded by the token issuer as CAT4MOQ actions and scopes.

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
own independent upstream graph. Two services on the same physical server can
peer with completely different upstream servers.

Tree topologies remain the simplest deployment model. A service may also contain
a relay cycle when every relay in that cycle negotiates the `RELAY_HOPS` setup
option. moqx assigns one random Hop ID to the running relay context, retains the
origin-to-relay path on namespace advertisements, appends its own ID when
forwarding, and drops an advertisement if its own ID is already present. Its
wildcard upstream subscription also excludes its own Hop ID.

```
          relay-a ─┐
          relay-b ──── relay-root (no upstream)
relay-c ──── relay-d ─┘
relay-e ─┘
```

Publishers and subscribers can connect to any relay. Subscriptions are forwarded
up toward the root as needed, and data flows back down to wherever subscribers
are.

Relay-hops negotiation is automatic between supporting moqx peers and requires
no YAML setting. A non-supporting publisher or origin is represented by one
stable, random stand-in Hop ID for the lifetime of its session; when an
advertisement next crosses a negotiated relay link, the outgoing path is
`[stand-in-origin, local-relay]`. Non-negotiated subscribers continue to receive
the legacy namespace format without hop parameters.

Loop prevention is only end-to-end across links that negotiated `RELAY_HOPS`.
Keep any graph segment containing a legacy relay acyclic, because a legacy relay
cannot preserve the path needed to recognize a returning advertisement.

### Limitations

- **Single upstream per service.** Each service can have at most one
upstream. Multiple upstreams, failover, or load balancing across upstreams are
not currently supported.
- **Legacy relay segments must remain acyclic.** Negotiated moqx peers detect
returning namespace advertisements, but a non-supporting relay cannot carry the
hop path through the graph.
- **No upstream authentication.** Upstreams are connected without any
application-level credential exchange beyond TLS.
- **Upstream connection is per-service, not per-track.** A single upstream
session is shared for all tracks within a service; there is no per-track
upstream routing.

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
