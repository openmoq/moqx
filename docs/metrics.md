# moqx Metrics

moqx exposes a Prometheus metrics endpoint on the admin HTTP server.

## Endpoint

```
GET /metrics
```

**Content-Type:** `text/plain; version=0.0.4; charset=utf-8`

The response uses [Prometheus exposition format v0.0.4](https://prometheus.io/docs/instrumenting/exposition_formats/).
All metric names are prefixed with `moqx_`.

## Naming Conventions

| Kind | Prometheus type | Name pattern |
|------|----------------|--------------|
| Counter | `counter` | `moqx_<name>_total` |
| Gauge | `gauge` | `moqx_<name>` |
| Histogram | `histogram` | `moqx_<name>_microseconds` |
| Error breakdown | `counter` | `moqx_<name>_by_code_total{code="<label>"}` |

**Role prefixes** on MoQ metrics:
- `pub*` — relay acting as **publisher** (serving downstream subscribers)
- `sub*` — relay acting as **subscriber** (consuming from upstream publishers)
- `moq*` — unambiguously tied to one role (no prefix needed)

## MoQ Application Layer — Counters

### Publisher-side

| Metric | Description |
|--------|-------------|
| `moqx_pubSubscribeSuccess_total` | Downstream SUBSCRIBE requests accepted (SUBSCRIBE_OK sent) |
| `moqx_pubSubscribeError_total` | Downstream SUBSCRIBE requests rejected (SUBSCRIBE_ERROR sent) |
| `moqx_pubFetchSuccess_total` | FETCH requests accepted (FETCH_OK sent) |
| `moqx_pubFetchError_total` | FETCH requests rejected (FETCH_ERROR sent) |
| `moqx_pubPublishNamespaceSuccess_total` | PUBLISH_NAMESPACE_OK received from subscriber |
| `moqx_pubPublishNamespaceError_total` | PUBLISH_NAMESPACE_ERROR received from subscriber |
| `moqx_pubPublishNamespaceDone_total` | PUBLISH_NAMESPACE_DONE sent |
| `moqx_pubPublishNamespaceCancel_total` | PUBLISH_NAMESPACE_CANCEL received |
| `moqx_pubSubscribeNamespaceSuccess_total` | SUBSCRIBE_NAMESPACE_OK sent |
| `moqx_pubSubscribeNamespaceError_total` | SUBSCRIBE_NAMESPACE_ERROR sent |
| `moqx_pubUnsubscribeNamespace_total` | UNSUBSCRIBE_NAMESPACE received |
| `moqx_pubPublishDone_total` | PUBLISH_DONE sent to a downstream subscriber |
| `moqx_pubSubscriptionStreamOpened_total` | Subscription streams opened to downstream |
| `moqx_pubSubscriptionStreamClosed_total` | Subscription streams closed to downstream |
| `moqx_pubTrackStatus_total` | TRACK_STATUS requests handled |
| `moqx_pubRequestUpdate_total` | REQUEST_UPDATE messages received |

### Subscriber-side

| Metric | Description |
|--------|-------------|
| `moqx_subSubscribeSuccess_total` | Outgoing SUBSCRIBE accepted upstream (SUBSCRIBE_OK received) |
| `moqx_subSubscribeError_total` | Outgoing SUBSCRIBE rejected upstream (SUBSCRIBE_ERROR received) |
| `moqx_subFetchSuccess_total` | FETCH_OK received from upstream |
| `moqx_subFetchError_total` | FETCH_ERROR received from upstream |
| `moqx_subPublishNamespaceSuccess_total` | PUBLISH_NAMESPACE_OK sent to upstream |
| `moqx_subPublishNamespaceError_total` | PUBLISH_NAMESPACE_ERROR sent to upstream |
| `moqx_subPublishNamespaceDone_total` | PUBLISH_NAMESPACE_DONE received from upstream |
| `moqx_subPublishNamespaceCancel_total` | PUBLISH_NAMESPACE_CANCEL sent to upstream |
| `moqx_subSubscribeNamespaceSuccess_total` | SUBSCRIBE_NAMESPACE_OK received from upstream |
| `moqx_subSubscribeNamespaceError_total` | SUBSCRIBE_NAMESPACE_ERROR received from upstream |
| `moqx_subUnsubscribeNamespace_total` | UNSUBSCRIBE_NAMESPACE sent upstream |
| `moqx_subPublishDone_total` | PUBLISH_DONE received from upstream publisher |
| `moqx_subSubscriptionStreamOpened_total` | Subscription streams opened from upstream |
| `moqx_subSubscriptionStreamClosed_total` | Subscription streams closed from upstream |
| `moqx_subTrackStatus_total` | TRACK_STATUS requests sent upstream |
| `moqx_subRequestUpdate_total` | REQUEST_UPDATE messages sent upstream |

### PUBLISH handshake

The relay can act as a publisher by sending PUBLISH to an upstream; it can also
receive PUBLISH from an upstream publisher.

| Metric | Description |
|--------|-------------|
| `moqx_pubPublishSuccess_total` | Relay sent PUBLISH and received PUBLISH_OK back |
| `moqx_pubPublishError_total` | Relay sent PUBLISH and received PUBLISH_ERROR back |
| `moqx_subPublishSuccess_total` | Relay received PUBLISH and sent PUBLISH_OK back |
| `moqx_subPublishError_total` | Relay received PUBLISH and sent PUBLISH_ERROR back (rejected) |

## MoQ Application Layer — Gauges

| Metric | Description |
|--------|-------------|
| `moqx_moqActiveSessions` | Active MoQ sessions |
| `moqx_pubActiveSubscriptions` | Active downstream subscriptions |
| `moqx_pubActivePublishers` | Active publishers registered via PUBLISH (relay is publisher) |
| `moqx_pubActivePublishNamespaces` | Active PUBLISH_NAMESPACE streams (relay is publisher) |
| `moqx_pubActiveSubscribeNamespaces` | Active SUBSCRIBE_NAMESPACE streams (relay is publisher) |
| `moqx_pubActiveSubscriptionStreams` | Active subscription streams to downstream |
| `moqx_subActiveSubscriptions` | Active upstream subscriptions |
| `moqx_subActivePublishers` | Active upstream publishers (accepted via PUBLISH_OK) |
| `moqx_subActivePublishNamespaces` | Active PUBLISH_NAMESPACE streams (relay is subscriber) |
| `moqx_subActiveSubscribeNamespaces` | Active SUBSCRIBE_NAMESPACE streams (relay is subscriber) |
| `moqx_subActiveSubscriptionStreams` | Active subscription streams from upstream |

## MoQ Application Layer — Histograms

Latency from request to response, in microseconds.
Buckets: 10, 50, 100, 250, 500, 1000, 2000, 5000, 10000, 50000, 100000, +Inf.

Each histogram is exported as three series:

```
moqx_<name>_microseconds_bucket{le="<bound>"}
moqx_<name>_microseconds_sum
moqx_<name>_microseconds_count
```

| Base name | Description |
|-----------|-------------|
| `moqx_moqSubscribeLatency_microseconds` | Time from SUBSCRIBE request to SUBSCRIBE_OK/ERROR response (subscriber role) |
| `moqx_moqFetchLatency_microseconds` | Time from FETCH request to FETCH_OK/ERROR response (subscriber role) |
| `moqx_moqPublishNamespaceLatency_microseconds` | Time from PUBLISH_NAMESPACE to PUBLISH_NAMESPACE_OK/ERROR (publisher role) |
| `moqx_moqPublishLatency_microseconds` | Time from PUBLISH to PUBLISH_OK/ERROR (publisher role) |

## Error Code Breakdowns

For each error counter listed below, an additional labeled series is exported
that breaks the total down by `RequestErrorCode`:

```
moqx_<name>_by_code_total{code="<label>"}
```

| code label | RequestErrorCode |
|------------|-----------------|
| `internal_error` | INTERNAL_ERROR |
| `unauthorized` | UNAUTHORIZED |
| `timeout` | TIMEOUT |
| `not_supported` | NOT_SUPPORTED |
| `track_not_exist` | TRACK_NOT_EXIST |
| `invalid_range` | INVALID_RANGE |
| `going_away` | GOING_AWAY |
| `cancelled` | CANCELLED (also used for unknown/future codes) |

Counters with per-code breakdowns:

- `moqx_pubSubscribeError_by_code_total`
- `moqx_pubFetchError_by_code_total`
- `moqx_pubPublishNamespaceError_by_code_total`
- `moqx_pubSubscribeNamespaceError_by_code_total`
- `moqx_pubPublishError_by_code_total`
- `moqx_subSubscribeError_by_code_total`
- `moqx_subFetchError_by_code_total`
- `moqx_subPublishNamespaceError_by_code_total`
- `moqx_subSubscribeNamespaceError_by_code_total`
- `moqx_subPublishError_by_code_total`

## QUIC Transport Layer — Counters

| Metric | Description |
|--------|-------------|
| `moqx_quicPacketsReceived_total` | UDP packets received |
| `moqx_quicPacketsSent_total` | UDP packets sent |
| `moqx_quicPacketsDropped_total` | UDP packets dropped |
| `moqx_quicPacketLoss_total` | Packet loss events detected |
| `moqx_quicPacketRetransmissions_total` | Packets retransmitted |
| `moqx_quicConnectionsCreated_total` | QUIC connections created |
| `moqx_quicConnectionsClosed_total` | QUIC connections closed |
| `moqx_quicStreamsCreated_total` | QUIC streams created |
| `moqx_quicStreamsClosed_total` | QUIC streams closed |
| `moqx_quicStreamsReset_total` | QUIC streams reset |
| `moqx_quicConnFlowControlBlocked_total` | Times a connection was blocked by connection-level flow control (mvfst only) |
| `moqx_quicStreamFlowControlBlocked_total` | Times a stream was blocked by stream-level flow control (mvfst only) |
| `moqx_quicCwndBlocked_total` | Times sending was blocked by the congestion window |
| `moqx_quicBytesRead_total` | Bytes read from the network |
| `moqx_quicBytesWritten_total` | Bytes written to the network |
| `moqx_quicDatagramsDroppedOnWrite_total` | QUIC datagrams dropped on write |
| `moqx_quicDatagramsDroppedOnRead_total` | QUIC datagrams dropped on read |
| `moqx_quicPeerMaxUniStreamsLimitSaturated_total` | Times the peer's unidirectional stream limit was reached (mvfst only) |
| `moqx_quicPeerMaxBidiStreamsLimitSaturated_total` | Times the peer's bidirectional stream limit was reached (mvfst only) |

## QUIC Transport Layer — Gauges

| Metric | Description |
|--------|-------------|
| `moqx_quicActiveConnections` | Active QUIC connections |
| `moqx_quicActiveStreams` | Active QUIC streams across all connections |

## Per-Track Metrics

```
GET /metrics/track?service=<name>&namespace=<a/b>&track=<name>&limit=<N>
```

Requires `admin.track_metrics_enabled` (default true); when it is false the
counting filters are never installed and this endpoint returns `503`.

Reports counters for **live tracks** — a track's series disappear when the track
goes away, and start from zero if it comes back.

| Parameter | Required | Description |
|---|---|---|
| `namespace` | yes | Namespace prefix in the safe form below. Matches every track under the prefix unless `track` is given. |
| `service` | no | Restrict to one service. Default: all services, each labeled. |
| `track` | no | Exact track name within the namespace. |
| `limit` | no | Max tracks to report. Default `admin.track_metrics_limit` (10), clamped to `admin.track_metrics_max_limit` (1000). |

`limit` is a sanity guard, not a top-N selector. A query matching more tracks
than the limit returns **400** with the match count rather than truncating —
an arbitrary subset would give Prometheus a series set that reshuffles between
scrapes, producing gap-filled graphs that look like data. Narrow the namespace
or raise the limit. A query that matches nothing returns 200 with no series.

Every series carries `{service, namespace, track}` labels.

### Name encoding

Namespaces and track names are arbitrary bytes, so the `namespace` and `track`
values — both in labels and in the query parameters — use the form RECOMMENDED
by moq-transport, [Representing Namespace and Track
Names](https://datatracker.ietf.org/doc/html/draft-ietf-moq-transport#name-representing-namespace-and-t).
The namespace `conf.example.com` / `room 1` renders as
`conf.2eexample.2ecom-room.201`.

Because the encoding is injective, two tracks can never collapse onto one label
set and produce duplicate series, and a scraped label value can be pasted
straight back into a query. Values outside the form — an unencoded `/` or space,
say — are rejected with 400 rather than silently matching something else.

| Metric | Type | Description |
|--------|------|-------------|
| `moqx_track_groups_received_total` | counter | Groups ingested |
| `moqx_track_subgroups_received_total` | counter | Subgroups ingested |
| `moqx_track_objects_received_total` | counter | Objects ingested |
| `moqx_track_datagrams_received_total` | counter | Objects ingested as datagrams (also counted in objects) |
| `moqx_track_bytes_received_total` | counter | Object payload bytes ingested |
| `moqx_track_groups_sent_total` | counter | Groups delivered, summed over subscribers |
| `moqx_track_subgroups_sent_total` | counter | Subgroups delivered, summed over subscribers |
| `moqx_track_objects_sent_total` | counter | Objects delivered, summed over subscribers |
| `moqx_track_datagrams_sent_total` | counter | Objects delivered as datagrams, summed over subscribers |
| `moqx_track_bytes_sent_total` | counter | Object payload bytes delivered, summed over subscribers |
| `moqx_track_subscribers` | gauge | Current downstream subscribers |
| `moqx_track_publish_start_timestamp_seconds` | gauge | Unix time the relay first saw the track |
| `moqx_track_last_object_timestamp_seconds` | gauge | Unix time of the most recent ingested object |

Group counters track the 3 most recently seen group IDs (LRU), so subgroups of
concurrently-open groups can arrive interleaved without inflating the count. A
group revisited after 3 *other* groups have been seen is counted again — the
window bounds per-filter state, and MoQ group IDs advance, so this only shows up
in pathological interleaving.

Byte counters measure **object payload bytes**, not wire bytes: no MOQT headers,
no QUIC or transport framing. For wire-level volume use `moqx_quicBytesRead_total`
and `moqx_quicBytesWritten_total`.

Sent counters are summed across subscribers, so one ingested object fanned out to
three subscribers increments `objects_sent` by 3.

### Bitrate and object rate

Rates are not computed server-side — derive them from the counters:

```promql
rate(moqx_track_bytes_received_total[1m]) * 8    # ingest bits/sec
rate(moqx_track_objects_received_total[1m])      # ingest objects/sec
rate(moqx_track_bytes_sent_total[1m]) * 8        # egress bits/sec (all subscribers)
```
