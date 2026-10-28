<!-- Version headings are setext h1, sections ATX h2; MD003 has no style for that. -->
<!-- markdownlint-disable-file MD003 MD041 -->
<!-- markdownlint-configure-file { "no-duplicate-heading": { "siblings_only": true } } -->

All notable changes to moqx are recorded here.
The format follows [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html).
How to add an entry: see [CONTRIBUTING.md](/CONTRIBUTING.md#changelog).

[Unreleased]
============

## Added

- Admin API: [`/qlog/capture`](/docs/logging.md#on-demand-capture) qlogs the next N new mvfst connections on a running relay. ([#779](https://github.com/openmoq/moqx/pull/779))
- [`cluster`](/docs/config.md#cluster) config: `enabled` turns MoQ Cluster negotiation off, and `hop_id` sets a stable Hop ID, with 0 for an anonymous relay. ([#837](https://github.com/openmoq/moqx/pull/837))
- A service can peer with several relays through an [`upstreams`](/docs/config.md#upstreams) list, and each upstream link takes a `relay_cost`. `/state` reports a list's links under `upstreams`. ([#838](https://github.com/openmoq/moqx/pull/838))
- A release cut with the `version release` workflow's `optimize` input ships a profile-guided, link-time-optimized `linux-amd64` relay, in the tarball and the image, that uses less CPU at the same throughput. See [PGO and LTO](/BUILD.md#pgo-and-lto). ([#804](https://github.com/openmoq/moqx/pull/804))

## Changed

- **Breaking:** an [`upstream`](/docs/config.md#upstreams) connection uses MoQT draft 18, so the upstream relay's listener must include 18 in `moqt_versions`. Peering also carries tracks that publishers push with PUBLISH. ([#833](https://github.com/openmoq/moqx/pull/833))
- **Breaking:** [MoQ Cluster](/docs/moq-cluster.md) ([draft-01](https://datatracker.ietf.org/doc/draft-lcurley-moq-cluster/01/)) replaces relay hops. Advertisements now carry `ROUTE_COST`. Loop protection requires every relay in a cycle to run this version. ([#836](https://github.com/openmoq/moqx/pull/836))
- With [`strict_claims`](/docs/config.md#authentication-and-authorization) on, the relay rejects a token that carries a claim other than `exp` and `moqt`, repeats a claim, or has an `exp` that is not an unsigned integer. ([#868](https://github.com/openmoq/moqx/pull/868))
- A slow subscriber could make the relay buffer without limit. [`max_subscription_buffer_kb`](/docs/config.md#services) sets the most each subscription may buffer, 5120 KB by default; past it the relay ends the subscription with `TOO_FAR_BEHIND`. 0 removes the limit. ([#866](https://github.com/openmoq/moqx/pull/866))

## Fixed

- The relay could crash when a new PUBLISH replaced a track that a TRACK_FILTER subscription was ranking. ([#823](https://github.com/openmoq/moqx/pull/823))
- Docker: the relay image starts with its default config under a non-root uid; `/var/log/moqx/qlog` is created writable. ([#827](https://github.com/openmoq/moqx/pull/827))
- With an [`upstream`](/docs/config.md#upstreams) configured, the relay could crash while waiting for the upstream connection or while shutting down. ([#821](https://github.com/openmoq/moqx/pull/821))
- With an [`upstream`](/docs/config.md#upstreams) configured, the relay could crash when it shut down just as the upstream connection completed. ([#825](https://github.com/openmoq/moqx/pull/825))
- The relay leaked memory each time a subscriber rejected a track that the relay pushed with PUBLISH. ([#831](https://github.com/openmoq/moqx/pull/831))
- After a publisher restarted a track, a FETCH that the relay answered from its cache stopped at the previous session's Largest Object and did not return the newer objects. ([#849](https://github.com/openmoq/moqx/pull/849))
- A track that a publisher pushed with PUBLISH had the same stale-Largest FETCH problem after a restart, and its publisher-set cache duration was ignored in favor of the [cache](/docs/config.md#cache) default. ([#850](https://github.com/openmoq/moqx/pull/850))
- [`upstream.tls.ca_cert`](/docs/config.md#upstreams) was ignored, so upstream certificates were checked against the system trust store. The relay now requires them to chain to that CA and match the URL's host, and it does not start if the file cannot be loaded. ([#865](https://github.com/openmoq/moqx/pull/865))
- The [cache](/docs/config.md#cache) kept gap markers of evicted groups on a long-lived track, using memory outside its size limit. ([#875](https://github.com/openmoq/moqx/pull/875))
- A client sending sparse FETCHes could grow the [cache](/docs/config.md#cache)'s gap markers without bound; each track now keeps at most 256. ([#880](https://github.com/openmoq/moqx/pull/880))

[0.3.6] - 2026-10-10
====================

- **Breaking:** dropped MoQT draft-14 support. ([#761](https://github.com/openmoq/moqx/pull/761))
  - A config with [`moqt_versions`](/docs/config.md#listeners) containing 14 fails at startup.

## Added

- Draft-18 SUBSCRIBE rendezvous timeout. ([#549](https://github.com/openmoq/moqx/pull/549), [#652](https://github.com/openmoq/moqx/pull/652), [#728](https://github.com/openmoq/moqx/pull/728))
- The picoquic listener uses all I/O threads. ([#721](https://github.com/openmoq/moqx/pull/721))
- Docker image: congestion control (`MOQX_CC`, `MOQX_PICO_CC`, `MOQX_BBR_SKIP_PROBE_RTT`) and qlog sampling (`MOQX_QLOG_SAMPLE`, `MOQX_QLOG_DIR`). ([#778](https://github.com/openmoq/moqx/pull/778))

## Changed

- Performance dashboard: the overview shows relay CPU, throughput per core, average latency, and RSS per session for each profile. ([#859](https://github.com/openmoq/moqx/pull/859))
- Docker image: the mvfst listener defaults to bbr2 congestion control (`MOQX_CC`). ([#801](https://github.com/openmoq/moqx/pull/801))
- A draft-18 SUBSCRIBE_NAMESPACE or SUBSCRIBE_TRACKS matching more than 1000 namespaces and tracks fails with NAMESPACE_TOO_LARGE. ([#667](https://github.com/openmoq/moqx/pull/667))
- `POST /cache/purge` purges one service, named by `service`; it is required when several services are configured. ([#794](https://github.com/openmoq/moqx/pull/794))
- Docker interop client image: offers drafts 18 and 16 only. ([#770](https://github.com/openmoq/moqx/pull/770))

## Fixed

- A thread could spin at 100% CPU during cache eviction. ([#720](https://github.com/openmoq/moqx/pull/720))
- A large object re-delivered in chunks tore down the subscription. ([#699](https://github.com/openmoq/moqx/pull/699))
- With `use_local_forwarders`, a track could lose its upstream subscription. ([#722](https://github.com/openmoq/moqx/pull/722))
- With `use_local_forwarders`, concurrent SUBSCRIBEs for one track could fail. ([#723](https://github.com/openmoq/moqx/pull/723))
- The relay requested objects from upstream with no forwarding subscriber. ([#742](https://github.com/openmoq/moqx/pull/742))
- The relay stopped reconnecting after an upstream dropped mid-handshake. ([#737](https://github.com/openmoq/moqx/pull/737))
- FETCH served from the cache could return wrong or missing objects. ([#734](https://github.com/openmoq/moqx/pull/734), [#735](https://github.com/openmoq/moqx/pull/735), [#736](https://github.com/openmoq/moqx/pull/736), [#750](https://github.com/openmoq/moqx/pull/750), [#755](https://github.com/openmoq/moqx/pull/755))
- Crashes in multi-threaded mode. ([#733](https://github.com/openmoq/moqx/pull/733), [#793](https://github.com/openmoq/moqx/pull/793))
- A shutdown error log for relays with an upstream. ([#746](https://github.com/openmoq/moqx/pull/746))
- A peer's namespace subscription outlived its session. ([#775](https://github.com/openmoq/moqx/pull/775))
- An upstream connect failure is logged only once. ([#744](https://github.com/openmoq/moqx/pull/744))
- With `use_local_forwarders`, a joining FETCH pipelined behind its SUBSCRIBE resolved against a stale or missing Largest. ([#783](https://github.com/openmoq/moqx/pull/783))
- FETCH served from the cache scrambled the payload of an object that arrived in three or more chunks. ([#808](https://github.com/openmoq/moqx/pull/808))
- Cache eviction could drop a live track's newest group, such as a catalog written once, so late joiners missed it. ([#739](https://github.com/openmoq/moqx/pull/739))
- picoquic WebTransport clients could not connect to the mvfst listener, which did not advertise `reset_stream_at`. ([#797](https://github.com/openmoq/moqx/pull/797))

## Dependencies

- moxygen [v0.3.6](https://github.com/openmoq/moxygen/releases/tag/v0.3.6)

[0.3.5] - 2026-09-11
====================

## Dependencies

- moxygen [v0.3.5](https://github.com/openmoq/moxygen/releases/tag/v0.3.5)

[0.3.4] - 2026-09-05
====================

- [Per-track QoS metrics](/docs/metrics.md#per-track-metrics) and dashboards. ([#539](https://github.com/openmoq/moqx/pull/539), [#640](https://github.com/openmoq/moqx/pull/640))
- `/state` is streamed and reports correct per-track counts. ([#627](https://github.com/openmoq/moqx/pull/627), [#630](https://github.com/openmoq/moqx/pull/630), [#632](https://github.com/openmoq/moqx/pull/632), [#633](https://github.com/openmoq/moqx/pull/633))

## Changed

- FETCH validates its requested range. ([#662](https://github.com/openmoq/moqx/pull/662))

## Fixed

- Crashes in multi-threaded mode. ([#653](https://github.com/openmoq/moqx/pull/653), [#654](https://github.com/openmoq/moqx/pull/654), [#677](https://github.com/openmoq/moqx/pull/677), [#683](https://github.com/openmoq/moqx/pull/683))
- FETCH served from the cache reported the wrong End Location. ([#678](https://github.com/openmoq/moqx/pull/678), [#679](https://github.com/openmoq/moqx/pull/679))
- The datagram `lastInGroup` flag was dropped. ([#681](https://github.com/openmoq/moqx/pull/681))
- SUBSCRIBE and FETCH failed past namespace nodes with no publisher. ([#684](https://github.com/openmoq/moqx/pull/684))
- FETCH preferred a namespace publisher over an exact-track upstream. ([#685](https://github.com/openmoq/moqx/pull/685))
- Draft-18 rejected empty namespaces. ([#687](https://github.com/openmoq/moqx/pull/687))

## Dependencies

- moxygen [v0.3.4](https://github.com/openmoq/moxygen/releases/tag/v0.3.4)

[0.3.1] - 2026-08-23
====================

## Fixed

- The release container image reports the release version. ([#637](https://github.com/openmoq/moqx/pull/637))

[0.3.0] - 2026-08-23
====================

- [Relay hops](/docs/relay-hops.md): namespace loops across relays are dropped. ([#502](https://github.com/openmoq/moqx/pull/502))
- [Per-track counters](/docs/metrics.md#per-track-metrics) at `/metrics/track`. ([#532](https://github.com/openmoq/moqx/pull/532), [#533](https://github.com/openmoq/moqx/pull/533), [#561](https://github.com/openmoq/moqx/pull/561))
- [Anonymous auth claims](/docs/config.md#anonymous-claim). ([#553](https://github.com/openmoq/moqx/pull/553))
- `/logs` admin endpoint. ([#491](https://github.com/openmoq/moqx/pull/491))

## Added

- [`/info`](/RUNNING.md#health-check) reports start time and uptime. ([#572](https://github.com/openmoq/moqx/pull/572))
- [`omit_metadata`](/docs/metrics.md#omitting-metadata) on the metrics endpoint. ([#604](https://github.com/openmoq/moqx/pull/604))
- Multiple AUTHORIZATION_TOKEN parameters per message. ([#552](https://github.com/openmoq/moqx/pull/552))
- Version reporting in the published artifacts. ([#505](https://github.com/openmoq/moqx/pull/505))

## Changed

- PUBLISH counters are named by relay role. ([#528](https://github.com/openmoq/moqx/pull/528))

## Fixed

- With `use_local_forwarders`, subscribers were accepted before the upstream answered. ([#545](https://github.com/openmoq/moqx/pull/545))
- With `use_local_forwarders`, a track's subscribers could split across forwarders. ([#546](https://github.com/openmoq/moqx/pull/546))
- A crash at shutdown. ([#587](https://github.com/openmoq/moqx/pull/587))
- The runtime Docker images lacked libevent. ([#598](https://github.com/openmoq/moqx/pull/598))

## Dependencies

- moxygen [v0.3.0](https://github.com/openmoq/moxygen/releases/tag/v0.3.0)

[0.2.1] - 2026-07-23
====================

There is no 0.2.0 release; this is the first release with these changes.

- Multi-threaded relay with [`threads`](/docs/config.md#top-level-structure). ([#361](https://github.com/openmoq/moqx/pull/361), [#362](https://github.com/openmoq/moqx/pull/362), [#364](https://github.com/openmoq/moqx/pull/364), [#365](https://github.com/openmoq/moqx/pull/365))
- Draft-18 SUBSCRIBE_TRACKS. ([#411](https://github.com/openmoq/moqx/pull/411))
- proxygen [qmux listener](/docs/config.md#listeners). ([#420](https://github.com/openmoq/moqx/pull/420))
- [CAT token authorization](/docs/config.md#authentication-and-authorization). ([#264](https://github.com/openmoq/moqx/pull/264), [#286](https://github.com/openmoq/moqx/pull/286), [#468](https://github.com/openmoq/moqx/pull/468))

## Added

- PKCS#12 TLS bundles. ([#460](https://github.com/openmoq/moqx/pull/460))
- [`GET /config`](/docs/config.md#endpoints) admin endpoint. ([#453](https://github.com/openmoq/moqx/pull/453))
- [`--logging` and `--log-handler`](/docs/logging.md) flags. ([#437](https://github.com/openmoq/moqx/pull/437))
- [qlog](/docs/logging.md#qlog-separately-for-structured-quic-analysis) for mvfst. ([#464](https://github.com/openmoq/moqx/pull/464))
- [mlog](/docs/config.md#loggingmlog) structured logging. ([#306](https://github.com/openmoq/moqx/pull/306))
- More listener options. ([#301](https://github.com/openmoq/moqx/pull/301), [#374](https://github.com/openmoq/moqx/pull/374), [#421](https://github.com/openmoq/moqx/pull/421))
- [QUIC transport metrics](/docs/metrics.md). ([#283](https://github.com/openmoq/moqx/pull/283), [#294](https://github.com/openmoq/moqx/pull/294), [#295](https://github.com/openmoq/moqx/pull/295))
- Subgroup-reset and object-ack-latency metrics. ([#462](https://github.com/openmoq/moqx/pull/462), [#478](https://github.com/openmoq/moqx/pull/478))
- Docker image: `MOQX_ENDPOINT` and a config template. ([#319](https://github.com/openmoq/moqx/pull/319), [#428](https://github.com/openmoq/moqx/pull/428))

## Changed

- The published image uses jemalloc and ships a stats stack. ([#480](https://github.com/openmoq/moqx/pull/480))
- A [max cache duration](/docs/config.md#cache) of 0 disables caching. ([#302](https://github.com/openmoq/moqx/pull/302))
- Default `moqt_versions` is 14 and 16; draft 18 is opt-in. ([#405](https://github.com/openmoq/moqx/pull/405))
- Bidirectional NAMESPACE forwarding requires draft 16+. ([#406](https://github.com/openmoq/moqx/pull/406))
- Faster UDP receive on mvfst. ([#337](https://github.com/openmoq/moqx/pull/337), [#350](https://github.com/openmoq/moqx/pull/350), [#371](https://github.com/openmoq/moqx/pull/371))
- [Log categories](/docs/logging.md#the-category-hierarchy) are rooted under `moqx.*`. ([#370](https://github.com/openmoq/moqx/pull/370))
- The datagram send queue is deeper and drops oldest-first. ([#450](https://github.com/openmoq/moqx/pull/450))

## Fixed

- The `quicActiveStreams` metric could go negative. ([#256](https://github.com/openmoq/moqx/pull/256))
- Crashes in the cache and in stats collection. ([#303](https://github.com/openmoq/moqx/pull/303), [#325](https://github.com/openmoq/moqx/pull/325))
- A memory leak in stats collection. ([#423](https://github.com/openmoq/moqx/pull/423))
- Shutdown could hang. ([#359](https://github.com/openmoq/moqx/pull/359))
- An unreadable TLS certificate aborted the relay. ([#434](https://github.com/openmoq/moqx/pull/434))
- A bad bind address gave no clean error. ([#461](https://github.com/openmoq/moqx/pull/461))
- With `use_local_forwarders`, races in track status, fetch and eviction. ([#443](https://github.com/openmoq/moqx/pull/443), [#445](https://github.com/openmoq/moqx/pull/445), [#451](https://github.com/openmoq/moqx/pull/451), [#452](https://github.com/openmoq/moqx/pull/452))

## Dependencies

- moxygen [v0.2.0](https://github.com/openmoq/moxygen/releases/tag/v0.2.0)
- catapult [2bbf479](https://github.com/Quicr/catapult/commit/2bbf479fe2e65e425624316d335443a8c0fc0507)

[0.1.4] - 2026-05-04
====================

The first tagged release.

- The relay, forked from moxygen's MoQRelay. ([#16](https://github.com/openmoq/moqx/pull/16))
- [Multi-service routing](/docs/config.md#services) by authority and path. ([#58](https://github.com/openmoq/moqx/pull/58))
- [Relay peering](/docs/config.md#upstreams) through per-service upstreams. ([#80](https://github.com/openmoq/moqx/pull/80))
- [Multiple listeners](/docs/config.md#listeners), each on mvfst or picoquic. ([#109](https://github.com/openmoq/moqx/pull/109), [#111](https://github.com/openmoq/moqx/pull/111), [#119](https://github.com/openmoq/moqx/pull/119), [#135](https://github.com/openmoq/moqx/pull/135))
- TRACK_FILTER top-N track selection. ([#160](https://github.com/openmoq/moqx/pull/160), [#161](https://github.com/openmoq/moqx/pull/161), [#162](https://github.com/openmoq/moqx/pull/162), [#164](https://github.com/openmoq/moqx/pull/164), [#165](https://github.com/openmoq/moqx/pull/165), [#166](https://github.com/openmoq/moqx/pull/166), [#169](https://github.com/openmoq/moqx/pull/169))
- [HTTP admin API](/docs/config.md#admin-server). ([#34](https://github.com/openmoq/moqx/pull/34), [#46](https://github.com/openmoq/moqx/pull/46), [#118](https://github.com/openmoq/moqx/pull/118), [#146](https://github.com/openmoq/moqx/pull/146), [#220](https://github.com/openmoq/moqx/pull/220))

## Added

- [YAML config](/docs/config.md).
- [QUIC transport settings](/docs/config.md#quic-settings) per listener. ([#113](https://github.com/openmoq/moqx/pull/113), [#131](https://github.com/openmoq/moqx/pull/131))
- Upstream NEW_GROUP_REQUEST forwarding. ([#149](https://github.com/openmoq/moqx/pull/149))
- [Cache config](/docs/config.md#cache). ([#140](https://github.com/openmoq/moqx/pull/140))
- [Prometheus metrics](/docs/metrics.md). ([#49](https://github.com/openmoq/moqx/pull/49), [#93](https://github.com/openmoq/moqx/pull/93), [#130](https://github.com/openmoq/moqx/pull/130), [#137](https://github.com/openmoq/moqx/pull/137))
- Graceful shutdown on SIGTERM and SIGINT. ([#34](https://github.com/openmoq/moqx/pull/34), [#112](https://github.com/openmoq/moqx/pull/112))
- Docker images on GHCR, and a rolling snapshot release. ([#38](https://github.com/openmoq/moqx/pull/38), [#40](https://github.com/openmoq/moqx/pull/40))

## Dependencies

- moxygen [v0.1.4](https://github.com/openmoq/moxygen/releases/tag/v0.1.4)
- yaml-cpp [0.8.0](https://github.com/jbeder/yaml-cpp/releases/tag/0.8.0)
- reflect-cpp [v0.18.0](https://github.com/getml/reflect-cpp/releases/tag/v0.18.0)
