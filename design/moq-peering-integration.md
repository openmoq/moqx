# Plan: moq-peering as an alternative peering backend for moqx

## Context

moqx peers relays today with MoQ Cluster (draft-lcurley-moq-cluster-01). Peers are MoQSessions dialed by
`UpstreamProvider`. Routes carry HOP_PATH/ROUTE_COST and live in `NamespaceTree`. That logic is spread across
`MoqxRelay`, `NamespaceTree`, `UpstreamProvider`, `MoqxRelayContext` and the three servers. No interface
separates it from the rest of the relay.

[quicr/moq-peering](https://github.com/quicr/moq-peering) (pinned `f1f8b3e`) is LAPS's relay-to-relay protocol.
It is a library, not MoQ:
- `PeerManager` owns picoquic sessions, node/subscribe info bases and SNS source routing. A via node forwards
  inside the library without the application.
- The application calls `Announce/Withdraw`, `Subscribe/UpdateSubscribe/Unsubscribe`, `SendData/EndSubgroup`.
- The library calls back through `Delegate::OnPeer{Subscribe,Unsubscribe,Namespace,Track,Data,StreamClosed,Up,Down}`
  on its own threads.
- Track identity is `HashTrack(TrackName)` (CityHash).
- The data payload is opaque bytes per (group, subgroup) stream or per datagram. LAPS puts MoQT subgroup-stream bytes
  there (header + objects, track_alias = track hash).

Goal: a relay can choose moq-peering instead of MoQ Cluster. Both sit behind a `PeeringBackend` interface. Decisions:
- One backend per relay, chosen in config.
- Object framing is MoQT encoding via moxygen's framer. Do not depend on libquicr.
- A backend only has to implement moxygen `Publisher`/`Subscriber` and drive `TrackConsumer`/`SubgroupConsumer`.
  It does not have to be MoQ underneath.

The stack lands as separate commits/PRs, in this order. It builds on the #676 stack (#932–#937), which takes
sessions out of relay state and is step 1.

## 1. Keep sessions out of relay state (done in #932–#937)

The #676 stack replaced every `shared_ptr<MoQSession>` in relay state and every `getRequestSession()` call. What
later steps build on, all in `src/relay/Participant.h` unless noted:
- `ParticipantInfo` is a value: `sessionId`, `version`, `executor`, `relayHops`, `peerHopID`, `linkCost`,
  `address`, `authority`. Relay code reads it on any thread.
- `Participant {info, publisher, subscriber}` is what the relay keeps. `publisher` and `subscriber` are callable
  from the relay's thread. Code already on `info->executor` calls `runOnExecutor(fn)` to skip the hop.
- `requestParticipant()` returns the requester. Relay entry points read it once and pass it down, and it XCHECKs
  outside a participant's request. `ParticipantScope` attaches a participant for a caller that a MoQ session did
  not start. `makeSessionId()` mints an id from the sequence that sessions use.
- `ParticipantSubscriptionHandle` attaches its participant to each REQUEST_UPDATE, which can carry a fill.
- `SessionParticipant` (`src/relay/SessionParticipant.h`) is the MoQ adapter. It is both handlers of one
  session, takes the `ParticipantInfo` snapshot on the session's executor, and holds the session weakly.
- `ExecBound<T>` (`src/relay/ExecBound.h`) holds a handle that an executor owns and releases it there.
- Enforcement: relay-core files include `relay/SessionConfined.h` last, which poisons `MoQSession`, `getSession`,
  `SessionParticipant` and related names. Relay entry points `XDCHECK(onRelayExec())`.

For the steps below, a peering backend is a `Participant`. It supplies its own `Publisher` and `Subscriber` and a
`ParticipantInfo` whose executor is the backend's and whose id comes from `makeSessionId()`. The relay does not
need a backend interface for requests or routes. A backend calls relay entry points on `relayExec_`, inside a
`ParticipantScope` for its participant. The handles that the relay keeps from a backend are `ExecBound` on the
backend's executor, so that executor has to outlive them.

## 2. `PeeringBackend` interface; move MoQ Cluster behind it (no behavior change)

`src/peering/PeeringBackend.h`, owned by `MoqxRelayContext`, one instance per service relay:
```cpp
class PeeringBackend {
 public:
  virtual ~PeeringBackend() = default;
  virtual void start(std::shared_ptr<MoqxRelay> relay, folly::EventBase* evb) = 0;
  virtual folly::coro::Task<void> stop() = 0;
  virtual bool hasPendingPeers() const = 0;                  // feeds waitForUpstreams
  virtual void visitState(RelayStateVisitor&) const = 0;     // onPeersBegin/onPeer/onPeersEnd
  virtual void visitContext(RelayContextVisitor&) const = 0; // onServiceUpstream(s)
  virtual bool isPeerRequest(const moxygen::TrackRequestParameters&) const = 0; // AuthFilters bypass
  // Runtime peer admin (step 8). Runs on the backend's executor.
  virtual void visitPeerMetrics(PeerMetricsVisitor&) const = 0;  // /metrics/peers (step 7)
  virtual folly::coro::Task<PeerAdminResult> adminPeer(PeerAdminOp op) = 0;
};
// PeerAdminOp{kind: Add|Remove|SetMetric|Restart, peer: string, metric: optional<uint64_t>}
// PeerAdminResult{ok, action, peer, metric, message} (same shape as moq::peering::PeerAdminResult)
```
`src/peering/MoqClusterPeering.{h,cpp}` gets what is MoQ Cluster–specific today. Line numbers are against the top
of the #676 stack (#937).
- `UpstreamProvider` creation and start: `MoqxRelayContext::initUpstreams/startUpstream`
  (`src/MoqxRelayContext.cpp:138-200`). `UpstreamProvider` already takes `relayExec` and wraps its handlers in a
  `SessionParticipant` per connection.
- The handshake: `onUpstreamConnectImpl` (`src/MoqxRelay.cpp:425`), `onUpstreamDisconnect` (`:473`) and
  `upstreamHandles_` (`src/MoqxRelay.h:486-491`). The handshake calls the upstream through `runOnExecutor`, and
  the handles are `ExecBound` on the upstream session's executor. `UpstreamProvider::stop()` fires the disconnect
  hook, so they are released before the IO threads stop.
- The reciprocal branch of `subscribeNamespaceImpl` (`src/MoqxRelay.cpp:1901-1960`) and `peerSubNsHandles_`
  (`src/MoqxRelay.h:493-505`). The relay calls a backend hook here instead of inlining it, and passes the
  requester's `Participant`.
- HOP_PATH/ROUTE_COST encode and decode: `setOutgoingHopPath` and `exclusionFor` (`src/MoqxRelay.cpp:83-140`)
  and `readClusterParams` (`:683`). These become a `RouteCodec` the backend installs. The codec reads
  `ParticipantInfo::relayHops`, `peerHopID` and `linkCost`, so the participant does not need anything new.
- The HOP_ID setup parameter on the three servers (`MoqxRelayServer.cpp:238`, `MoqxPicoRelayServer.cpp:90`,
  `MoqxQmuxRelayServer.cpp:87`). The context passes it only when the backend is MoQ Cluster.

`MoqClusterPeering` dials sessions and runs its handshake on their executors, so it may name `MoQSession`, like
`UpstreamProvider` and `MoqxRelayContext` do. It is not a `SessionConfined.h` file. The relay hooks that it calls
are relay entry points, so they run on `relayExec_`.

These parts stay in the relay because they are backend-neutral:
- `NamespaceTree` multi-route storage and `betterRoute`.
- `RouteExclusion`.
- `reconcileAdvertisement`.
- Echo suppression by peerID.

A route from a non-cluster backend gets `path={}`, `cost=0`, `fromCluster=false`, the same as a plain publisher.

Tests: move the `UpstreamProviderTest` lifecycle and `MoqxRelayPeerTests` handshake cases to drive
`MoqClusterPeering`. The Python `relay_chain`, `relay_hops_cycle`, `relay_cluster_diamond` and `relay_upstreams`
tests stay green.

## 3. Build: moq-peering dependency

- CPM-fetch `quicr/moq-peering` at a pinned SHA in `cmake/dependencies.cmake`. Turn off
  `MOQ_PEERING_BUILD_TESTS/EXAMPLES`, but build `moq_peering_relay` for the interop test in step 7.
- Gate it with `MOQX_ENABLE_MOQ_PEERING`, default OFF until the step 9 integration tests pass.
- picoquic: the library uses an existing `picoquic-core` target. Verify that moxygen/openmoq's picoquic is new
  enough for the library's pin `v2026.10.22.70b5fd5`; bump or patch if not.
- spdlog: CPM'd by moq-peering. It sets `SPDLOG_FMT_EXTERNAL` when `fmt::fmt` exists (folly's), so check for
  duplicate fmt symbols under san.
- Bridge spdlog into moqx logging: pass `Config::logger` with a sink that forwards to glog/mlog.

## 4. Config

A service gets `moq_peering:`, mutually exclusive with `upstream`/`upstreams`. When any service uses it,
`cluster.enabled` must be false; strict config makes a conflict an error.
```yaml
services:
  live:
    moq_peering:
      node_id: "edge-sea-1"        # hashed by the library; must be distinct
      node_type: edge              # edge | via | stub
      node_metric: 100
      listen_port: 33434
      peers: [{host: 10.1.2.3, port: 33434, metric: 150}]
      tls: {cert_file: ..., key_file: ...}
      check_interval_ms: 5000
      moqt_version: 16             # framing of object bytes inside SendData
```
- Add `ParsedMoqPeeringConfig` (`src/config/loader/ParsedConfig.h`) and `MoqPeeringConfig` (`src/config/Config.h`).
- Resolve in `ConfigResolver.cpp`, serialize in `ConfigSerializer.h`, document in `docs/config.md` and
  `config.example.yaml`.
- `MoqxRelayContext` builds a `MoqPeeringBackend` or a `MoqClusterPeering` per service.

## 5. `MoqPeeringBackend`: control plane

Files: `src/peering/MoqPeeringBackend.{h,cpp}`, plus `src/peering/PeerManagerApi.h`. The second file is a thin
virtual wrapper over `moq::peering::PeerManager` so unit tests can substitute a fake.

- Threading: the backend gets its own **peering EventBase thread**. It is not the relay thread and not a shared IO
  thread.
  - Why: calls into `PeerManager` can block.
    - `SendData(kNewStream)` opens a QUIC stream through `QuicConnection::Invoke` → `QuicEndpoint::Run`
      (`quic_transport.cc:153,472`).
    - From any thread except picoquic's network thread, `Run` posts the work and waits on a condition variable until
      that thread runs it.
    - The engine also takes a single `info_base_->mutex_` that the network thread contends for.
    - A new subgroup (often every group) would therefore stall whatever thread calls it. On the relay thread that
      stalls all control. On an IO thread it stalls every MoQ session on that evb.
  - The peering evb owns all backend state: the track_hash maps, import parsers, export consumers and the stats
    collector. Every `PeerManager` call happens there. Every `Delegate` callback copies its arguments and `add()`s
    onto it. Do not lock.
  - The backend's `ParticipantInfo::executor` is the peering evb. The existing cross-exec machinery then applies
    unchanged:
    - `maybeCrossExec`, `SubscriberCrossExecFilter` and `PublisherCrossExecFilter` move subscribe calls onto the
      peering evb and objects back to the forwarder's IO thread. This is the same as a session pinned to another IO
      evb.
    - Data does not touch the relay thread.
  - Control calls into the relay (`doPublishNamespace`/`Done`, namespace-subscriber registration) hop to `relayExec_`,
    the way `makeNamespaceBridgeHandle(..., relayExec)` does today, and run inside a `ParticipantScope` for the
    mesh participant. The relay's entry points assert `relayExec_`.
  - One peering thread per backend instance. The library's single picoquic network thread is the ceiling, so a
    second moqx-side thread does not buy throughput. If parse or serialize CPU becomes the limit, shard export and
    import by `track_hash` across a small pool. Leave that for later.
  - The follow-up section below injects moqx's picoquic EventBase transport into moq-peering. That removes both this
    thread and the blocking wait.
  - Shutdown order: withdraw the mesh participant's routes and drop its relay subscriptions while both executors
    run, so the relay releases its `ExecBound` handles on the peering evb. Then `SetDelegate(nullptr)`, destroy
    `PeerManager` on the peering evb, join the thread, and drop the backend.
- Participant: one `Participant` stands for the whole mesh.
  - `info` is `ParticipantInfo{.sessionId = makeSessionId(), .version = moqt_version, .executor = peering evb}`.
    It has no relay hops, HOP_ID or link cost.
  - `publisher` is the import Publisher from step 6, wrapped in `PublisherCrossExecFilter` that targets the peering
    evb. `subscriber` is the export side, wrapped in `SubscriberCrossExecFilter`. The bare objects are the
    on-executor pair that `runOnExecutor` hands out.
- Import of names:
  - `OnPeerNamespace(ns, withdrawn)` → `relay->doPublishNamespace(pubNs, meshParticipant, nullptr, "moq-peering")`
    / `doPublishNamespaceDone(ns, meshParticipant.info->sessionId)`.
  - `OnPeerTrack` → the same at track granularity, via the relay's PUBLISH-style track entry.
- Export of names:
  - The backend registers as a namespace subscriber for the empty prefix, with `forward=false` and the peer echo
    filter. NAMESPACE / NAMESPACE_DONE → `Announce/Withdraw(kNamespace)`. Track announcements → `Announce(kTrack)`.
  - Do not export routes whose participant is the backend itself. Loop prevention inside the mesh is the
    library's job.
- `track_hash` ↔ `FullTrackName` map: `HashTrack` on the way out, and the `TrackName` from `OnPeerSubscribe` on the
  way in.
- `OnPeerUp/Down` update `/state` peer rows from `Peers()`. The library withdraws routes learned through a session
  on its own.

## 6. `MoqPeeringBackend`: data plane

Why the bytes need framing:
- The peering data header (`src/peering/messages/data_header.h`) carries only type, SNS id, track hash, priority and
  TTL.
- `SendData`'s `group_id`/`subgroup_id` only key the local egress stream (`engine.cc:70`); they do not reach the wire.
- Stream bytes are forwarded as raw QUIC chunks (`Engine::ForwardPeerData`), so `OnPeerData` does not preserve
  message boundaries.

So the payload has to carry group, subgroup, object id, status and extensions, and be self-delimiting. MoQT subgroup
and datagram encoding through moxygen's existing writer and parser does all of this with no new codec, and LAPS uses
the same layout.

**Import.** The relay subscribes upstream through the mesh.
- `MoqPeeringPublisher::subscribe(SubscribeRequest, TrackConsumer)`:
  - Calls `PeerManager::Subscribe(name, params)`. Map priority, delivery timeout, forward, and new-group request.
  - Returns a synthesized SubscribeOk without largest. moq-peering does not have SUBSCRIBE_OK.
- `OnPeerData(hash, is_new_stream, stream_id, bytes)`:
  - Feed a per-`stream_id` moxygen object-stream parser (the codec behind `ObjectStreamCallback`) that drives
    `TrackConsumer::beginSubgroup` → `SubgroupConsumer`.
  - Datagrams (`stream_id` empty) parse as MoQT datagrams → `consumer->datagram()`.
  - Bytes arrive pipelined, so the parser must accept partial objects.
- `OnPeerStreamClosed(reset)` → `endOfSubgroup()` or `reset()`. The handle's `unsubscribe` → `PeerManager::Unsubscribe`.
- `requestUpdate` → `UpdateSubscribe`.
- `fetch`, `trackStatus`, `subscribeNamespace` return NOT_SUPPORTED. The relay cache still serves FETCH for objects
  it holds. Document the gap.

**Export.** A peer wants a track this relay can serve.
- `OnPeerSubscribe(name, hash, params)` → `SubscribeRequest{LargestObject, forward}` with a
  `MoqPeeringTrackConsumer`, sent to the relay inside a `ParticipantScope` for the mesh participant. Use one relay
  subscription per `track_hash`; the library already merges peers.
  - RelayExec mode: `PublisherCrossExecFilter(relayExec_, relay)` hops to `relayExec_` and wraps the consumer, so
    objects come back to the peering evb. The returned `CrossExecSubscriptionHandle` releases the relay's handle on
    `relayExec_`.
  - Open: `createPublisherFilter()` does not fit the other modes. In LocalForwarder mode it subscribes on the
    caller's executor through that thread's forwarder registry, which the peering evb does not have. In
    SingleThread mode it is the relay itself, so the backend has to hop to the relay's IO thread first.
  - Call the handle's `requestUpdate` inside the same `ParticipantScope`, because a fill needs the requester.
- `MoqPeeringTrackConsumer` / `SubgroupConsumer` serialize through `MoQFrameWriter` at `moqt_version`:
  - The first object of a (group, subgroup) → subgroup header + object, sent with `SendData(kNewStream)`.
  - Later objects → `kExistingStream`.
  - `endOfSubgroup` → `EndSubgroup(false)`, and `reset` → `EndSubgroup(true)`.
  - `datagram` → `kDatagram`.
- The track alias in headers is `track_hash`, masked to 62 bits. First verify how libquicr/LAPS writes the 64-bit hash
  as a varint, then match it.
- `OnPeerUnsubscribe(hash, subscribers_remain=false)` → unsubscribe the relay subscription.
- `publishDone` from upstream → `EndSubgroup` on any open streams, then drop the mapping.

## 7. Stats

Today stats come from four sources:
- **MoQ counters** (`pub*`/`sub*` in `STATS_MOQ_COUNTER_FIELDS`). moxygen's per-session stats callbacks fill them:
  `MoqxRelayContext.cpp:284` creates a `MoQStatsCollector` per worker evb, and `:306-307` attaches it to each
  session.
- **QUIC counters**. `QuicStatsCollector` and `PicoQuicStatsCollector` fill these from moqx's own servers.
- **Per-track counters**. `TrackStatsFilter` wrapping inside `MoqxRelay` fills these (`wrapWithTrackStats`,
  cpp:1196, 2637, 2799), and `/metrics/track` serves them with per-track labels.
- **`/state`**. It is a snapshot, not metrics.

A moq-peering backend is not a MoQSession, and its picoquic runs on the library's thread. So without extra work it
reports none of the MoQ or QUIC counters. Plan:

1. **Per-track stats come along for free.** Import and export both go through `relay->subscribe` and the route's
   Publisher, so `TrackStatsFilter` counts peered tracks the same as session tracks. Verify that in a test. Do not
   add per-track code to the backend.
2. **Backend counters as a new field group** in `StatsRegistry.h`. Do not mix them into `pub*`/`sub*`; those mean
   "MoQ sessions", and dashboards rely on that.
   - `STATS_PEERING_COUNTER_FIELDS`:
     - announces and withdraws sent/received
     - subscribes and unsubscribes sent/received
     - objects, datagrams and bytes sent/received
     - subgroup streams opened, closed and reset (sent and received)
     - framing parse errors
     - objects dropped because the parse failed or the track was unknown
     - peer up/down events
     - admin ops, with an ok/error breakdown
   - `STATS_PEERING_GAUGE_FIELDS`:
     - peers configured
     - peers connected
     - imported tracks (local subscribes into the mesh)
     - exported tracks (mesh subscribes served locally)
     - open peering streams
   - `STATS_PEERING_HISTOGRAM_FIELDS`:
     - `peeringRttSample` (from `PeerSnapshot::srtt_us`)
     - `peeringSubscribeLatency`: from `PeerManager::Subscribe` to the first `OnPeerData`. moq-peering does not have
       SUBSCRIBE_OK, so time to first object is the useful signal.
   - Add the group to `STATS_*_FIELDS` so `operator+=`, `formatPrometheus` and `/metrics` pick it up. Use the
     `moqx_peering_*` prefix.
3. **`PeeringStatsCollector : StatsCollectorBase`.** The backend owns it, its `owningExecutor()` is the peering evb,
   and it registers with `StatsRegistry::registerCollector`. Register the peering evb with an
   `EventBaseStatsCollector` too, so its loop busy/idle shows up next to the IO threads.
   - Event counters increment inline. They already run on that executor because every Delegate callback hops there
     first.
   - The connected-peer and stream gauges and the RTT sample come from `PeerManager::Peers()`, called inside
     `snapshot()` on the backend executor. The library does not offer a stats callback, so polling at scrape time is
     the only source.
4. **The MoQ Cluster backend uses the same collector** for peer up/down, configured/connected peers, reconnect
   attempts (`UpstreamProvider::reconnectLoop`) and admin ops. Its sessions are real MoQSessions, so they keep
   filling the MoQ and QUIC counters as they do today. A dashboard can then show peer health without knowing which
   backend is running.
5. **Per-peer metrics at `GET /metrics/peers`**, served like `/metrics/track` (`src/admin/TrackMetricsHandler.cpp`).
   - Labels: `service`, `backend`, `peer` (host:port or URL), `node_id`, `origin` (dialed/accepted).
   - Series:
     - `moqx_peer_connected`
     - `moqx_peer_srtt_microseconds`
     - `moqx_peer_metric`
     - `moqx_peer_connect_time_seconds`
     - `moqx_peer_subscribe_tracks`
     - `moqx_peer_publishes`
     - `moqx_peer_active_streams`
   - For moq-peering every series comes from `Peers()`. For MoQ Cluster: connected, metric = relayCost and connect
     time come from `UpstreamProvider`, and srtt and streams come from the session transport where available.
6. **QUIC detail is missing.** `Peers()` does not include loss, bytes, cwnd or congestion counters. Draft an
   upstream moq-peering change: a `PeerSnapshot` byte/packet/loss block, or a stats callback hook. Wire it into the
   collector when it lands. Until then, document the gap.

Tests:
- `PeeringStatsCollector` unit tests: events → counters. `Peers()` from a fake → gauges, histogram and `/metrics/peers`
  series.
- `StatsRegistry` aggregation, and the Prometheus golden output for the new group.
- Python: after `relay_moq_peering_pair` delivers N objects, `/metrics` shows the `moqx_peering_objects_*` counts,
  `/metrics/track` shows the track on both relays, and `/metrics/peers` shows one connected peer.

## 8. Admin endpoints: add, remove, reweight and restart peers

Add `src/admin/PeerAdminHandler.{h,cpp}`, registered next to `registerCachePurgeRoute`
(`src/admin/CachePurgeHandler.cpp:24`) and built the same way:
- Parse the JSON body synchronously.
- `co_withExecutor` on the admin EVB, with cancellation.
- Call `MoqxRelayContext::adminPeer(service, op)`, which hops to that service's backend executor.
- Map the result to an HTTP status.

| Route | Body | Effect |
| --- | --- | --- |
| `POST /peers/add` | `{"service","peer","metric"?}` | dial a new peer |
| `POST /peers/remove` | `{"service","peer"}` | disconnect it and drop its routes |
| `POST /peers/metric` | `{"service","peer","metric"}` | change the peer's weight |
| `POST /peers/restart` | `{"service","peer"}` | force a reconnect |

Status codes:
- 200 returns `{ok, action, peer, metric, message}`.
- 400 for a bad body or a missing field.
- 404 for an unknown service, or a peer that does not exist on remove, metric or restart.
- 409 for a duplicate add.
- 503 before the context is ready.

Each backend parses its own `peer` string.

**moq-peering** (`peer` = `host:port`, IPv6 bracketed). Each op maps one-to-one onto
`PeerManager::AddPeer/RemovePeer/SetPeerMetric/RestartPeer`, through `PeerManagerApi`. Translate the library's
`PeerAdminResult` directly: `ok=false` with "duplicate" maps to 409, and "does not exist" maps to 404.

**moq-cluster** (`peer` = upstream URL, e.g. `moqt://host:port/path`). `MoqClusterPeering` keeps its
`UpstreamProvider`s in a map keyed by normalized URL instead of the startup-only list.
- Add: build and start an `UpstreamProvider` the way `startUpstream` does. TLS settings come from the service's
  upstream TLS config, or `insecure`/`ca_cert` in the body.
- Remove:
  - `provider->stop()`.
  - Run `onUpstreamDisconnect` for its session, so its routes and handles go.
  - Erase it from `MoqxRelay::upstreamProviders()`.
- Metric: set `relayCost`. RELAY_COST is a SETUP parameter, so the new value takes effect when the peer reconnects.
  The response says so; pass `"restart": true` to reconnect in the same call.
- Restart: close the provider's current session. `reconnectLoop` dials again, with its backoff reset.

Runtime changes are not written back to config. `/config` keeps showing the file. `/state` (`onPeer` /
`onServiceUpstreams`) shows the live set, including each peer's current metric and connect state. The new routes are
mutating, so they get the same exposure as `POST /cache/purge`. Document them in `docs/admin.md`, or wherever
`/cache/purge` is documented.

Tests:
- `test/admin/PeerAdminHandlerTest.cpp`: body parsing and status mapping against a fake backend.
- `MoqPeeringBackendTest`: each op reaches `FakePeerManager` with the right arguments.
- `MoqClusterPeering` tests:
  - Add starts a provider and runs the peer handshake.
  - Remove withdraws that peer's routes.
  - Metric + restart re-sends SETUP with the new RELAY_COST.
  - Restart reconnects.
- Python:
  - In `relay_cluster_diamond`, raise one leg's metric through `/peers/metric` with restart. New subscriptions then
    take the other leg.
  - In `relay_moq_peering_pair`, remove then re-add the peer. Delivery resumes.

## 9. Tests and verification

Unit tests in `test/MoqPeeringBackendTest.cpp`, using `FakePeerManager` (a `PeerManagerApi`) and the existing
`MoqxRelayTestFixture`:
- Announce in both directions.
- Echo suppression.
- Subscribe import: fake `OnPeerData` with moxygen-encoded subgroup bytes, split at odd boundaries → consumer sees the
  objects.
- Export: local publisher objects → expected `SendData`/`EndSubgroup` sequence and bytes.
- Datagram both ways.
- Peer down → routes withdrawn.
- Shutdown with callbacks in flight.

Framing round-trip: encode with the export consumer, parse with the import parser, at every supported `moqt_version`.

Config: resolver and serializer cases for `moq_peering`, plus the exclusivity errors.

Integration, in Python through `test/lib/moq_harness.py`; add `relay(..., moq_peering=...)` and ports in
`test/test_ports.sh`:
- `relay_moq_peering_pair`: two moqx edges, publisher on A, subscriber on B. Assert delivery and that unsubscribe tears
  down.
- `relay_moq_peering_via`: moqx edge → `moq_peering_relay --type via` (built from the dep) → moqx edge. The library's
  own via forwards, so this tests SNS routing and wire compatibility with the reference implementation.
- The stats cases from step 7 and the admin cases from step 8.

Run the commands from memory:
- `./scripts/configure.sh san --moxygen from-source --moxygen-dir /home/afrind/develop/submodules/moxygen` with
  `-DMOQX_ENABLE_MOQ_PEERING=ON`
- `./scripts/dev/format.sh`
- `./scripts/build.sh san`
- `./scripts/test.sh san`

Steps 1–2 must leave the full suite and the Cluster Python tests green before step 3 starts.

## Follow-up: run moq-peering on moqx's picoquic EventBase servers

The goal is to drop the dedicated peering thread and the library's private picoquic network thread. moq-peering
connections would then run on moqx's IO threads, multiplexed with MoQT on the same UDP socket. This needs changes in
both moq-peering and moxygen, so it is a separate stack after steps 1–9.

**moq-peering (upstream PR).** Split `QuicEndpoint` (`src/peering/quic_transport.cc`) behind a transport interface:
```cpp
class PeeringTransport {        // moq::peering, public header
  virtual void Dial(host, port, ConnectionHandler&) = 0;
  virtual void SetAcceptHandler(std::function<ConnectionHandler*(PeeringConnection&)>) = 0;
  virtual void Post(std::function<void()>) = 0;   // run on the thread that owns these connections
  virtual bool OnOwningThread() const = 0;
};
class PeeringConnection {       // one QUIC connection: streams, datagrams, srtt, close
  ...
};
```
- `Config` gains `std::shared_ptr<PeeringTransport> transport`. When it is unset, the library keeps today's
  self-owned picoquic network thread, so LAPS sees no change.
- `Invoke`/`Run`, which block today, run inline when called on the owning thread. With an injected transport the
  caller is on that thread, so the condition-variable wait goes away.
- `Engine` already funnels per-connection work through `Invoke`/`Dispatch`. The change is mostly in
  `quic_transport.cc` and `peer_session.cc`; `engine.cc` keeps its forwarding logic.
- `Engine::CheckThread` (the reconnect timer, `engine.cc:1217`) becomes a timer posted on the transport.
- One `info_base_->mutex_` still guards the engine. If several moqx IO threads own peering connections, they contend
  on it. Accept that for v1. Splitting per connection is a larger change for the library owners to decide.

**moxygen.**
- `PicoProtocolDispatcher` (`moxygen/openmoq/transport/pico/PicoProtocolDispatcher.h`) routes by ALPN to `Quic` or
  `WebTransportH3`. Add a pluggable entry so an embedder can register `"moq-peering"` and get the raw picoquic
  connection on the shard's EventBase.
- `MoQPicoQuicShardedServer` / `MoQPicoQuicEventBaseServer` and `MoQPicoQuicEventBaseClient` expose:
  - the accepted connection
  - an outbound dial for a non-MoQT ALPN
  - the shard's `PicoQuicExecutor` as the owning thread
- `PicoQuicStatsCallback` then covers peering connections too, which closes the QUIC-stats gap in step 7.

**moqx.**
- A `PicoPeeringTransport : moq::peering::PeeringTransport` wraps the shard server.
  - Accepts arrive on whichever IO thread owns the connection.
  - Dials go to a chosen shard.
  - `Post` uses that shard's executor.
- `MoqxPicoRelayServer` adds `moq-peering` to its ALPN list when a service enables moq-peering. Peering then shares
  the service's listener port, and `listen_port` becomes optional.
- The backend's own state still needs one owner. Keep the backend's control plane on one chosen IO evb (or the
  relay exec) and treat Delegate callbacks from other shards like any cross-exec hop. Alternatively, shard the
  backend's per-track maps by connection owner if the library ends up with per-connection callbacks.
- The step 5 peering thread then goes away, and stats come from the existing `PicoQuicStatsCollector` per IO thread.
- Requires running moqx with the pico server for that service. The mvfst server cannot host it.

**Verification.**
- The same `relay_moq_peering_pair` and `relay_moq_peering_via` tests run with the injected transport, plus a case
  where MoQT clients and a peering link share one port.
- Thread checks (`DCHECK(evb->isInEventBaseThread())`) in `PicoPeeringTransport` catch any remaining off-thread call.

## Known gaps to document (`docs/moq-peering.md`)

- moq-peering does not support FETCH or joining FETCH upstream; FETCH works only from the local cache.
- Track status is not carried.
- SUBSCRIBE_OK does not carry largest location.
- Location filters reduce to "latest".
- The node id is a string hash, not the `<high>:<low>` form.
- The library does not validate peer certificates; it sets no trust anchor. Note this for deployments.
- Peers added through admin are not persisted; a restart reverts to the config file.
- QUIC transport counters (loss, bytes, cwnd) for peering connections are unavailable until moq-peering exposes them;
  only srtt and stream counts are reported.
