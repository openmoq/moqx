# Plugins: Level 1 API

Decisions for the first implementation of [plugins.md](plugins.md). Auth, routing (plain chaining and MoQ Cluster) and an example data-plane plugin are its users.


## Instances

| Instance | One per | Holds | Created by |
| --- | --- | --- | --- |
| Root | `plugins:` entry | Config, process-wide state (a Hop ID, key material) | The host, from the registered factory |
| Service instance | Root × applicable service | Per-service state and the service's view of the config | Its root |
| Session state | Session × plugin | What `admit` returned (grants, negotiated flags) | The plugin, returned from `admit` |

- A process-scope entry applies to every service. A service-scope entry applies to its service.
- The host keeps session state on the session and passes it to that plugin's request hooks.
- Hooks run on the executor of their call site. Each hook documents which one.
- Session and request hooks run on the session's I/O worker. One service instance serves every worker, so these hooks run concurrently.
- Route hooks run on the service's relay executor.
- Shared plugin state is immutable after `start()` or synchronized by the plugin.
- A plugin may start threads. Only `attach`, `detach` and `Continuation::resolve` are callable from them. Everything else goes back to the owning executor.

## Lifecycle

- Root `start()` runs on the main EventBase before listeners start, in config order.
- Service `start()` runs on the service's relay executor, after the relay exists and before it accepts sessions, in config order.
- `stop()` runs in reverse: services, then roots. The host awaits every `stop()` before draining executors.
- A failed `start()` aborts startup with its error.
- `start()` and `stop()` are coroutines. `start()` runs under a cancellation the host fires at shutdown; `stop()` does not.
- Startup and shutdown are the only lifecycle events. Services are not added or removed at runtime.

## Configuration and registration

```yaml
plugins:                      # process scope
  - type: moq-cluster
    id: cluster               # optional; defaults to type
    rank: 10                  # optional; defaults to 0
    config:
      hop_id: 1001
      upstreams:              # per service, keyed by service name
        live:
          - url: "moqt://origin.example.com:4433/moq-relay"
            relay_cost: 1
services:
  live:
    plugins:                  # service scope
      - type: cat4moq-auth
        config:
          token_type: 16
          hmac_keys: [{id: cat-dev, secret: "..."}]
      - type: simple-router   # only when no process-scope router applies
        config:
          upstreams:
            - url: "moqt://origin.example.com:4433/moq-relay"
```

- `plugins:` entries replace the `auth:`, `cluster:`, `upstream:` and `upstreams:` sections. Old configs fail validation with a message naming the replacement.
- Upstream connections are router configuration. A service-scope router lists them directly; a process-scope router keys them by service name.
- A chain runs process entries first, then service entries, each sorted by `rank`, ties in list order.
- `id` is unique per scope. It names the plugin in logs, `/state` and errors.
- Each builtin registers in [src/plugins/Builtins.cpp](/src/plugins/Builtins.cpp): type name, factory, a reflect-cpp config struct, and a redacting serializer. The loader reads the envelope and hands the `config` subtree to the plugin's struct, so `moqx --validate` and `/config` keep typed errors and redaction.
- Registration is one explicit list. Static-constructor registration is rejected: an archive can drop an unreferenced translation unit.
- Validate-only mode runs the factories. Factories have no side effects.

## Types

- Control-plane hooks see moqx-owned views. A view borrows the moxygen message for the duration of the call and exposes typed accessors. Parameters are read lazily: `authTokens(tokenType)`, `uintParam(key)`, `bytesParam(key)`. Nothing is copied when no hook reads it.
- A plugin that produces parameters returns a small moqx struct. The host inserts it into the outgoing message and validates it in that message's frame and draft.
- Control-plane hooks return `Result<T>` = `folly::Expected<T, Error>`. `Error{Kind, reason}` has one `Kind` enum: `Unauthorized`, `Forbidden`, `ExpiredToken`, `MalformedToken`, `TooManyTokens`, `DoesNotExist`, `Internal`. The host maps `Kind` to the message's moxygen error code and fills the request ID.
- Data-plane hooks are moxygen types: `TrackConsumer`, `SubgroupConsumer`, `FetchConsumer`, `ObjectHeader`, `Payload`. A filter subclasses `moxygen::TrackConsumerFilter` and `SubgroupConsumerFilter`.
- No plugin header on the control plane includes moxygen. Plugins use folly directly.

## Control plane

Every control-plane hook is a coroutine (`folly::coro::Task`). A hook that never suspends completes inline, so a synchronous policy costs no executor hop.

Session hooks, on the session's I/O worker:

```text
setupParams(scope)                       -> Task<Params>            // SETUP parameters to declare; scope = listener | upstream
admit(SetupView)                         -> Task<Result<SessionState>>
sessionEnded(SessionState)               -> Task<void>
```

Request hooks, on the session's I/O worker:

```text
authorize(RequestView, SessionState)     -> Task<Result<void>>
```

- `RequestView` carries the request kind, namespace, track, lazy parameters, the session view and `peer`.
- `peer` is host-owned: the request carries the relay peering token. Auth keeps its bypass for `SUBSCRIBE_NAMESPACE` and `SUBSCRIBE_TRACKS` exactly as narrow as today.
- For `SUBSCRIBE`, `FETCH` and `PUBLISH`, `RequestView::filters` is the request's attach slot. See [Attach](#attach).
- Each plugin declares the hooks it implements. The host builds each chain from those only. A service whose plugins implement no request hook installs no filter on its sessions.

Snapshot hook, on the relay executor:

```text
stateSnapshot()                          -> Task<folly::dynamic>
```

- Each plugin's snapshot appears in `/state` under `plugins.<id>`. The host prints what it owns: subscriptions, cache, tracks.

Admission:

- moxygen gains `validateAuthorityAsync()` on `ServerSetupCallback`, returning `Task<Expected<Unit, SessionCloseErrorCode>>`. Its default calls the synchronous `validateAuthority()`, which stays pure virtual. Setup stays incomplete until it resolves.
- Traffic that arrives meanwhile is held, bounded by count and byte limits. Exceeding a bound closes the session.
  - The control stream: the codec stops after `CLIENT_SETUP` and keeps the unparsed suffix (`ParseResult::BLOCKED`), so a request in the same read is not dispatched. The read loop resumes the codec on acceptance. A FIN seen while blocked is replayed.
  - Request bidi streams (uni-control versions): parked before their first read, so the data stays in the transport and QUIC flow control throttles the peer. Count limit on parked streams.
  - Data uni streams: the existing pre-setup buffer.
  - Datagrams after `CLIENT_SETUP`: buffered raw, count and byte limit. Before `CLIENT_SETUP` they still close the session.
- No deadline in moxygen. The check is host code, and the host bounds `admit` with its own timeout. A check that never resolves holds the session until the peer disconnects.
- A rejected admission closes the session with the returned `SessionCloseErrorCode`. Before draft 18, `SERVER_SETUP` is sent only on acceptance.
- The check starts inline on the session executor. A plugin that only verifies a local token completes without suspending, and the wire is unchanged for it.
- `PUBLISH` returns a deferred consumer with `consumerReady=false`; the host points it at the relay once `authorize` resolves.
- Checks run under the session's cancellation. The host checks for cancellation before dispatch, so a check that ignores cancellation never reaches the relay.

## Router

The host knows no routes. It knows the source it is subscribed to for each track, and nothing about why.

- One router per service. A service that configures none gets `simple-router`.
- The router owns: announcements and their lifetime, namespace-interest matching and advertisement, upstream connections, relay peering, request resolution, and whatever state those need. Paths, costs, origins and ranking rules never cross into the host.
- The host owns: forwarders and fanout, the subscription registry (one source per track), cache, rendezvous parking, Top-N, request and session lifetimes, executor dispatch, cancellation, and the wire.

Router hooks, on the relay executor:

```text
announcement(AnnouncementView, Endpoint)        -> Task<Result<AnnouncementLease>>   // PUBLISH_NAMESPACE; lease.update(), lease.end()
interest(InterestView, AdvertisementSink)       -> Task<Result<InterestLease>>       // SUBSCRIBE_NAMESPACE, SUBSCRIBE_TRACKS
resolve(RequestView)                            -> Task<Result<Resolution>>          // SUBSCRIBE, FETCH, TRACK_STATUS with no forwarder; ready(SourceLease) | missing | refused
mayUse(RequestView, SourceLease)                -> Task<Result<bool>>                // may this requester join the track's existing source
endpointEnded(EndpointId)                       -> Task<void>
```

- `SourceLease` is an opaque identity, the owner executor, a `Publisher` capability, and lifetime state. The host subscribes, fetches and asks track status on it. Replacement or disconnect invalidates the lease and the host's subscriptions on it.
- `mayUse` guards an existing subscription: a cluster peer may be refused a source that runs through it, even when another route is eligible. The host calls it before a requester joins a forwarder, in every mode, including a ready local forwarder.
- `AdvertisementSink` is `advertise(ns, params)` and `withdraw(ns)`. The host turns them into NAMESPACE and NAMESPACE_DONE on the right stream and owns the handles. Which subscriber hears about which namespace, replay on a new interest, and update suppression are the router's.
- `availabilityChanged(ns)` is a host capability the router calls when a namespace or track may now resolve. Rendezvous parking stays in the host and wakes on it. A wait is armed before `resolve` so a notification during a suspended resolution is not lost.
- After a suspension the host rechecks registry epochs and the router revalidates its own bindings.
- Upstream connections, the relay token, the reciprocal `SUBSCRIBE_NAMESPACE` and `SUBSCRIBE_TRACKS`, reconnect and cleanup live in one relay-peering library both routers use.
- The `peer` flag on `RequestView` stays host-owned and request-local.

## Local endpoints

A plugin can be a publisher and a subscriber inside its service.

```text
ServiceContext::localEndpoint(pluginId)  -> LocalEndpoint
endpoint.announce / publish / subscribe / fetch   // into the service, through normal dispatch
endpoint.setPublisher / setSubscriber             // the host's requests to the endpoint
endpoint.close()
```

- An endpoint has an identity, an owner executor, request handles and a cancellation. It is not a `MoQSession`; the relay's request paths take an explicit context instead of `getRequestSession()`.
- Endpoint requests run the control-plane chain like any other, with principal `local:<pluginId>` and no session state. `cat4moq-auth` admits a local principal. Other plugins see and may refuse them.
- Closing an endpoint withdraws its announcements, ends its published tracks, and cancels its subscriptions and fetches.
- A router uses `SourceLease`, never an endpoint, for upstream-sourced tracks.

## Data plane

Filters are moxygen consumer decorators. Each call returns before the host continues unless the filter holds the object.

Rules the host enforces:

- Payload bytes are immutable. A filter forwards the `Payload` it received or drops the object.
- Location (`group`, `subgroup`, `id`), `priority`, `status` and immutable extensions are fixed. A filter that changes them fails the stream.
- Mutable extensions may be added, changed or removed.
- Dropping an object in a subgroup stream resets that stream downstream. Dropping an object stream or a datagram drops that object.
- A header filter never sees chunks, so it keeps the declared length.

### Attach

Configured filters do not exist. The control plane decides what to hook, when.

```text
RequestView::filters.reserve()              -> Slot           // SUBSCRIBE, FETCH, PUBLISH
SessionView::filters(stage).reserve()       -> Slot           // everything this session sends or receives
ServiceContext::trackFilters(track, stage)  -> Slot           // every consumer of the track, existing and future
slot.attach(filter, rank)                   -> Attachment
attachment.detach()
```

- A slot not reserved costs nothing: the host hands moxygen the bare consumer.
- A reserved slot costs one version check per stream open. Objects inside a subgroup bypass it.
- `attach` and `detach` are callable from any thread. They take effect at the next `beginSubgroup`, the next object stream or datagram. A fetch's chain is fixed when it starts.
- Order on one consumer: session scope, then track scope, then request scope, then the relay, on ingress. Egress mirrors it. Within a scope, ascending rank, then attach order. Egress runs descending rank.
- Reservation must happen before the consumer reaches moxygen or a forwarder. `authorize` is the place for a request slot. A track slot wraps consumers created after it, and reaches existing consumers when the host's command lands on their executor.
- In LocalForwarder mode, egress filters run on the subscriber's executor and ingress filters on the publisher's. The passive relay copy observes; it cannot change delivery.

### Hold

```text
holder.hold(event)         -> Continuation   // the filter keeps the event; the host queues the stream behind it
continuation.resolve(event | drop)           // any thread; the host re-injects on the owning executor
```

- The host keeps one queue per stream. In-stream order is preserved. Across streams nothing is ordered.
- `endOfSubgroup`, `endOfFetch` and `publishDone` wait for the stream's pending holds. `reset` drops them and later resolutions are no-ops.
- The queue is bounded per stream. Exceeding the bound resets the stream.
- A held event is one record. Resolving off the owning executor adds one executor task.

## Builtin plugins

| Type | Scope | Hooks |
| --- | --- | --- |
| `cat4moq-auth` | service | `admit`, `authorize` |
| `simple-router` | service, implicit | Router hooks, `stateSnapshot`. Longest-prefix lookup, last announcer wins, configured upstreams, anonymous paths. Today's behavior with MoQ Cluster off. |
| `moq-cluster` | process | `setupParams`, `admit`, router hooks, `stateSnapshot`. Adds HOP_ID, HOP_PATH and ROUTE_COST, loop drop, path ranking, exclusion. |
| `extension-tagger` | service | `authorize` reserves a slot and attaches a filter that adds a mutable extension per object on egress for sessions matching its config. Exercises request, session and track scope. |

- Behavior of auth, chaining and cluster is unchanged. The mesh tests keep passing, with `/state` readers moved to `plugins.<router>`.
- Routes, peers and upstream connection state appear under the router's snapshot. The host's `/state` keeps subscriptions, cache and tracks.

## Layout

```text
src/plugin/            API, chains, slots, hold queue, config envelope   (moqx_plugin)
src/plugins/Builtins.cpp
src/plugins/cat4moq-auth/
src/plugins/peering/        relay-peering library shared by the routers
src/plugins/simple-router/
src/plugins/moq-cluster/
src/plugins/extension-tagger/
```

- Each plugin is a static library. A fork adds a directory and one line in `Builtins.cpp`.

## Delivery

A stack of branches, each a PR on the one below it. Every step keeps today's behavior and tests.

1. moxygen (openmoq fork): codec block/resume, then `validateAuthorityAsync` with held traffic and bounds. moqx builds against the checkout with `scripts/configure.sh --moxygen-dir` until the fork merges it.
2. moqx: API, registry, config envelope, lifecycle, `cat4moq-auth`.
3. moqx: `SourceLease` and an explicit request context replace `MoQSession` in the subscription registry and the relay's request paths. Routing still in the host.
4. moqx: local endpoints.
5. moqx: router extraction. `simple-router` carries the host's routing and peering as they are; MoqxRelay loses the namespace tree. Top-N indexing splits off the tree first.
6. moqx: `moq-cluster` router; `cluster:` and `upstream(s):` move under router config; `/state` migration; harness readers move.
7. moqx: attach slots, hold queue, `extension-tagger`.
