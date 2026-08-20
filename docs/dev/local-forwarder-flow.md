# Local-Forwarder Data Plane

In **LocalForwarder (LF) mode** the relay runs across multiple I/O threads. The core idea is
to keep the expensive per-subscriber **delivery** fan-out on the publisher's and subscribers'
own executors, so it never runs on the central relay executor. `relayExec_` is not bypassed
entirely, though: it still carries a *single* passive copy of every object for relay-global
concerns (the cache and Top-N ranking), plus all lifecycle/control events.

Three kinds of traffic move between the executors:

- **Subscriber delivery** flows *outward* from the publisher's forwarder, on the publisher and
  subscriber threads — off `relayExec_`. This is the N-way fan-out we want to keep off the
  hot central thread.
- **One passive object copy** flows to `relayExec_`, feeding the cache and Top-N ranking — one
  copy per object regardless of subscriber count.
- **Lifecycle/control events** (`onEmpty`, `forwardChanged`, `newGroupRequested`) flow through
  the callback chains, either to relay state on `relayExec_` or back to the publisher on
  `[Pub]` depending on which forwarder fired.

See [Data flow](#data-flow) for how objects move, and [Control](#control-the-callback-flow) for
the callback chains.

## Modes

`createPublisherFilter()` / `createSubscriberFilter()` pick the per-session handler based on
mode:

| Mode | Publish entry | Subscribe entry | Delivery data plane |
| --- | --- | --- | --- |
| `SingleThread` | `publish()` | `subscribe()` | calling thread |
| `RelayExec` | `publish()` (on `relayExec_`) | `subscribe()` (on `relayExec_`) | `relayExec_` |
| `LocalForwarder` | `LocalPublishFilter::publish` → `publishFromPublisherExec` | `LocalSubscribeFilter::subscribe` → `subscribeFromSubscriberExec` | publisher/subscriber execs |

> Naming convention: `…FromPublisherExec` / `…FromSubscriberExec` is an entry point that
> *starts* on that executor; `…OnRelayExec` is a continuation that *runs* on `relayExec_`.

Executor legend used below:

```
[Pub]   the publisher forwarder's exec (local publisher's exec, or the upstream session's
        exec when the track is relay-sourced)
[Relay] relayExec_
[Sub]   the subscriber session's exec
──▶  direct call        ⇢⇢▶  executor hop (co_withExecutor / fire-and-forget)
```

---

## Publish

A publisher's local forwarder **is** the publisher forwarder — a single forwarder serves the
whole track, living on the publisher's executor rather than on `relayExec_`. Ownership lives in
`tlForwarders_` on the publisher's executor, **not** in the registry: the registry entry keeps
its subscription/namespace bookkeeping plus a *remote* `ForwarderRef` — weak + owner executor,
which hands out no pointer at all (`getIfOwned()` returns null for it) — so `relayExec_` can
neither read the forwarder nor run `~MoQForwarder`. Relay-side decisions about a forwarder travel
to its executor as messages (`ForwarderRef::post`), never as pointers. The entry is named by a
`SubscriptionRegistry::EntryEpoch` rather than by the forwarder, so a caller that suspends can
tell its own entry from one a reconnecting publisher put in its place.

### Setup

```
[Pub]   LocalPublishFilter::publish()
[Pub]    └─▶ publishFromPublisherExec()
[Pub]         ├─▶ make_shared<MoQForwarder>(ftn)              # bare; the Claim sets its initial state
[Pub]         ├─▶ installPublisherForwarder(ftn, fwd, InstallKind::FromPublish)
[Pub]         │     ├─ setCallback( LocalForwarderCallback → CrossExec → Weak )  # removeOnEmpty=false
[Pub]         │     └─ tlForwarders_->replaceAndPark(ftn, fwd)  # → {claim, displaced}: the Claim plus
[Pub]         │                                                 #   the parked occupant's identity
[Pub]         ├─▶ claim.markReady(InitialTrackState{largest, extensions})
[Pub]         ├─▶ localPubFwd->addChannelSubscriber(passive)  # passive copy → [Relay]
[Pub]         ├─▶ co_invoke(reply) ⇢⇢▶ [Relay]               # runs in the background
[Pub]         └─▶ return {consumer, replyTask}                # immediate; the publisher can write now
[Relay]            └─▶ registerPublishOnRelayExec()
[Relay]                 ├─▶ publishWithSession()              # registry (entry holds remote ForwarderRef) + namespace tree
[Relay]                 │     └─▶ addSubscriberAndPublish()   # attach existing SUBSCRIBE_NAMESPACE subs (see Publish fan-out)
[Relay]                 └─▶ relayChainFilter->setDownstream(topNFilter)
[Pub]         reply resumes ─▶ takeDisplaced(ftn, displaced)  # release the parked occupant
```

The publisher starts writing into `localPubFwd` on `[Pub]` as soon as `publishFromPublisherExec`
returns; `[Relay]` registration completes in parallel. FIFO ordering on `[Relay]` guarantees
`relayChainFilter` is wired to `topNFilter` before any tapped object arrives.

`installPublisherForwarder` also installs the forwarder's control chain (publisher forwarder →
relay state); see [Control](#control-the-callback-flow).

### Duplicate publisher (eviction)

When a publish displaces a live entry, the old forwarder must be drained (its subscribers would
otherwise wait forever) — but it lives on its own executor, which may not be this one. Two
mechanisms cooperate, both identity-keyed:

- **The drain is a message routed by the evicted entry's `ForwarderRef`**
  (`evicted.forwarder.post(publishDone)` in `publishWithSession`): it runs on the old
  forwarder's own exec and touches exactly the evicted forwarder, never whatever now occupies a
  slot.
- **Parking anchors the lifetime while the answer travels.** `replaceAndPark` keeps the
  displaced occupant's last ref on its own thread; the publish reply — always resumed on the
  publisher exec by `MoQSession::handlePublish` — releases it with
  `takeDisplaced(ftn, displaced)`. The release is keyed by the displaced occupant's identity, so
  concurrent same-track publishes each reclaim exactly what their own call parked. The park
  never drains: any parked occupant that is not the evicted publisher's forwarder reaches
  `publishDone` through its own source-termination cascade.

---

## Publish fan-out

A session that sent `SUBSCRIBE_NAMESPACE` receives a `PUBLISH` for each matching track. When a
track appears (or a namespace-subscriber arrives), the relay adds that session as a subscriber
to the forwarder and starts publishing to it. This is publish-side flow, not the
track-`subscribe()` path — but the LF variant mirrors `subscribeFromSubscriberExec`:
`acquireLocalForwarder` → `startPublish` → buffer-and-replay → a `[Pub]` sortie to wire the
channel sub, preceded by a resolve sortie because the dispatcher hands over only a `TrackRef`
(name + owning exec), never the forwarder. The difference is the entry point (dispatched from
`[Relay]` to the namespace-subscriber's `[Sub]`) and that there is no upstream/relay-chain setup
here — that is already owned by the publisher's forwarder.

```
[Relay] addSubscriberAndPublish(publisherRef)              # dispatcher (LF variant vs startPublish)
[Relay]  └─ LF mode ─▶ ⇢⇢▶ [Sub] addSubscriberAndPublishViaLocalForwarder(ref.track())
[Sub]         ├─ fast path: tlForwarders_->getIfReady(ftn) hit
[Sub]         │     └─▶ startPublish() ─▶ awaitPublishReply()   # zero hops
[Sub]         ├─▶ ⇢⇢▶ [Pub] resolvePublisherOnItsExec()         # getIfReady + capture InitialTrackState
[Sub]         │     └─ no forwarder ─▶ return                   # torn down before the hop landed
[Sub]         ├─▶ acquireLocalForwarder(ftn, initial)           # shared with subscribe
[Sub]         ├─▶ startPublish()                                # issue PUBLISH to the sub
[Sub]         ├─ NOT new ─▶ awaitPublishReply() ─▶ return
[Sub]         └─ isNew ── this thread owns setup:
[Sub]              ├─▶ setCallback( PendingForwarderCallback )   # buffer events during setup
[Sub]              ├─▶ CrossExecFilter(subscriberExec, localFwd) # deep-copy per thread
[Sub]              ├─▶ publisherRef.co_with() ⇢⇢▶ [Pub]
[Sub]              │     ├─▶ buildLocalToPublisherCallbacks()
[Sub]              │     └─▶ installChannelSubscriber(localFwd ↔ publisherFwd)
[Sub]              ├─ no finalCallback (publisher gone) or sawOnEmpty?
[Sub]              │     └─▶ teardownLocalForwarderOnFailure() ─▶ return
[Sub]              ├─▶ replayPendingFowarderEvents()             # drain buffered events
[Sub]              └─▶ awaitPublishReply()
```

Callers of `addSubscriberAndPublish` (all on `[Relay]`): `publishWithSession` (publisher
arrives → attach existing namespace-subscribers), `subscribeNamespaceImpl` (namespace-subscriber
arrives → attach existing publishes), `onTrackSelected` (ranking promotes a track).

---

## Subscribe (track `subscribe()`)

Subscribe is a three-executor dance: it starts on `[Sub]`, hops to `[Relay]` for registry +
upstream work, and nests a single sortie to `[Pub]` to wire the channel sub.

### Setup

```
[Sub]   LocalSubscribeFilter::subscribe()
[Sub]    └─▶ subscribeFromSubscriberExec()
[Sub]         ├─▶ localRegistry().join(ftn)                     # serializes same-thread races
[Sub]         │
[Sub]         ├─ Ready   ─▶ attachSubscriber() ─▶ return        # fast path, zero hops
[Sub]         ├─ Pending ─▶ await the entry, re-resolve, attach # another setup owns it
[Sub]         │
[Sub]         └─ Claim ── this thread owns setup:
[Sub]              ├─▶ setCallback( PendingForwarderCallback )   # buffer events during setup
[Sub]              ├─▶ CrossExecFilter(subscriberExec, localFwd) # deep-copy per thread
[Sub]              ├─▶ localFwd->addSubscriber()                 # so `forward` is right pre-hop
[Sub]              └─▶ ⇢⇢▶ [Relay] attachNewLocalForwarderOnRelayExec()
[Relay]                 ├─▶ joinOrPrepareUpstreamSubscription()  # registry: first vs subsequent
[Relay]                 │     ├─ FIRST: refMaker finds the publisher session and builds the entry's
[Relay]                 │     │         remote ForwarderRef; none ⇒ NoPublisher, nothing created,
[Relay]                 │     │         DOES_NOT_EXIST
[Relay]                 │     └─ SUBSEQUENT: await the first subscriber, take its publisherExec
[Relay]                 └─▶ ⇢⇢▶ [Pub]  single sortie (the only strong-ref scope):
[Pub]                        ├─ if SUBSEQUENT: publisherFwd = tlForwarders_->getIfReady(ftn)
[Pub]                        │              # gone (publisher left or was displaced) ⇒ INTERNAL_ERROR
[Pub]                        ├─ if FIRST: another forwarder already occupies the slot (a PUBLISH
[Pub]                        │              raced this SUBSCRIBE) ⇒ INTERNAL_ERROR
[Pub]                        ├─ if FIRST: installPublisherForwarder(InstallKind::FromSubscribe)
[Pub]                        │              # chain + tl entry; publisher Claim held across setup
[Pub]                        ├─▶ buildLocalToPublisherCallbacks()
[Pub]                        ├─▶ installChannelSubscriber(localFwd ↔ publisherFwd)
[Pub]                        ├─ if FIRST subscriber:
[Pub]                        │    ├─▶ addChannelSubscriber(relayChain, passive)
[Pub]                        │    ├─▶ subscribeUpstreamAndApplyOk()
[Pub]                        │    └─▶ publisherClaim.markReady(InitialTrackState{ok})  # on OK only
[Pub]                        └─▶ attach.initial = InitialTrackState::capture(publisherFwd)
[Relay]                 ◀── back on relayExec_
[Relay]                 ├─ if FIRST: relayChainFilter->setDownstream(topNFilter)
[Relay]                 └─ if FIRST: completeUpstreamSubscription() pending.complete()
[Sub]              ◀── back on subscriberExec (tail)
[Sub]              ├─ sawOnEmpty? ─▶ teardownLocalForwarderOnFailure()  # via attach.publisherRef
[Sub]              ├─ error?      ─▶ teardownLocalForwarderOnFailure(localFwd)  # + publishDone;
[Sub]              │                                                 #   ~Claim vacates the entry
[Sub]              ├─▶ claim.setInitialState(attach.initial)
[Sub]              ├─▶ replayPendingFowarderEvents()             # drain buffered events
[Sub]              └─▶ claim.markReady() ─▶ return sub           # releases parked joiners
```

The first subscriber does the heavy lifting (upstream subscribe + installing the relay chain,
the same passive cache/Top-N chain described in [Data flow](#data-flow)). Subsequent
subscribers on the same thread hit the `attachSubscriber` fast path; on other threads they take
the non-first half of the sortie: resolve the live publisher forwarder from `tlForwarders_` on
the publisher exec, install the channel sub, and read its established largest/extensions.

The strong ref to the publisher forwarder exists only inside the sortie: the relay-side frame
carries it in, the tl entry takes ownership, and on every exit — success, error, or throw — the
frame's ref dies on the owning exec. What escapes to `[Relay]` and the tail is
`attach.publisherRef`, a remote `ForwarderRef` used only to post teardown messages.

### Why each guard exists

- **`join` before the hop** — serializes same-thread subscribers so only one gets the `Claim`
  and runs setup; the rest get `Ready` (attach) or `Pending` (await, then re-resolve).
- **The `Claim` held across setup** — the entry stays unattachable until the upstream OK has
  established `largest`, so a joiner never reads a value a client would see as a track restart.
  Its destructor fails the entry on every exit that does not mark it ready.
- **`PendingForwarderCallback` installed first** — events firing mid-setup are buffered, then
  replayed by `replayPendingFowarderEvents` so none are lost across the hops.
- **`addSubscriber` before the hop** — `numForwardingSubscribers()` must be correct when
  `addChannelSubscriber` runs on `[Pub]`, so the `forward` flag is right from the start.
- **Single `[Pub]` sortie** — merges channel-sub install + relay chain + upstream subscribe
  into one round-trip instead of bouncing `[Relay]↔[Pub]` twice.
- **`sawOnEmpty` check in the tail** — all subscribers may cancel during the hops; if so,
  unwind the channel subs cleanly. The error path unwinds through the same call, additionally
  draining `localFwd` with `publishDone`.

---

## Data flow

Once setup is done, every object the publisher writes goes to three kinds of destination. The
first two are delivery (off `relayExec_`); the third is the single passive copy for
relay-global state.

```
[Pub]  publisher writes object ──▶ publisher forwarder (localPubFwd)
[Pub]    ├──▶ same-thread subscribers                          # direct, fast path
[Pub]    ├──▶ CrossExecFilter ⇢⇢▶ [Sub]  localFwd ──▶ consumer # one hop per other thread, deep-copy
[Pub]    └──▶ relayChainFilter (passive) ⇢⇢▶ [Relay]           # cache + Top-N (below)
```

The cross-exec delivery copies payloads (`deepCopyPayload=true`) so each subscriber thread owns
its own IOBuf chain. Sharing one chain would mean every thread's `IOBuf` add/release writes the
same atomic refcount, bouncing that cache line between CPUs; a per-thread copy keeps those
refcount writes CPU-local and off the shared line.

### Objects on relayExec_: caching and Top-N

Subscriber *delivery* stays off `relayExec_`, but the relay still needs a single coherent view
of every object for two relay-global concerns — the **cache** and **Top-N ranking** — both of
which live on `relayExec_`. The publisher's forwarder carries one passive channel subscriber
that copies every object inward:

```
[Pub]  localPubFwd / publisherFwd dispatches object
        └──▶ relayChainFilter (CrossExecFilter, passive) ⇢⇢▶ [Relay]
[Relay]      └──▶ topNFilter ──▶ TerminationFilter ──▶ cache passive consumer
```

This chain is built in the LF branch of `buildFilterChain`:

- `relayChainFilter` is a `CrossExecFilter` targeting `relayExec_`; its downstream is set to
  `topNFilter` once the registry entry exists (in `registerPublishOnRelayExec` for the publish
  path, or `attachNewLocalForwarderOnRelayExec` for a relay-sourced track's first subscriber).
- The chain ends at `cache_->makePassiveConsumer(ftn)` (or a `NullTrackConsumer` when the
  cache is disabled).

It is added with `forward=true, passive=true`: the chain observes **every** object, but does
not count as a forwarding subscriber and is excluded from the `onEmpty` quorum — so the
publisher's `onEmpty` still fires when the last *real* subscriber leaves.

What each stage does:

- **`TopNFilter`** — reads the ranking property from each object's extensions and updates
  `PropertyRanking`. Ranking is relay-global, so it must observe the full object stream on one
  thread; this is what decides which tracks are "selected" for track-filter subscribers. See
  [track-filter-ranking.md](track-filter-ranking.md).
- **`TerminationFilter`** — intercepts `publishDone` to clean up relay state.
- **cache passive consumer** — populates the cache so later subscribers / fetches can be
  served from it.

The cost is one copy per object, independent of subscriber count; the per-subscriber fan-out
is what the delivery data plane keeps off `relayExec_`.

---

## Control: the callback flow

A `MoQForwarder` fires three control callbacks: `onEmpty` (last real subscriber left),
`forwardChanged` (forwarding-subscriber count crossed zero), and `newGroupRequested` (a
subscriber issued `NEW_GROUP_REQUEST`). In LF mode each forwarder wraps these in a short chain
of adapter callbacks so the event reaches the right thread and the right owner. There are
**two** distinct chains, depending on which forwarder fired. (Single-thread mode skips all of
this: `forwarder.callback = MoqxRelay` directly.) See the `MoQForwarder::Callback chain
overview` comment block in `MoqxRelay.cpp` for the canonical wiring.

The reusable adapter layers:

| Callback | Runs on | Role |
| --- | --- | --- |
| `PendingForwarderCallback` | forwarder's exec | buffers events during the setup window, replayed onto the real callback afterward |
| `LocalForwarderCallback` | forwarder's exec | owns removal from `tlForwarders_` (identity-checked); passes the rest through |
| `CrossExecForwarderCallback` | hops | dispatches each event to a target exec, fire-and-forget |
| `WeakRelayForwarderCallback` | `[Relay]` | terminal for the publisher chain: recovers the relay, calls the `…Impl` methods |
| `ChannelForwarderCallback` | `[Pub]` | terminal for the local-forwarder chain: detaches the channel sub / issues `requestUpdate` |

### Publisher forwarder → relay state

Built by `installPublisherForwarder`, on both the publish and subscribe paths. Lifecycle events
on the publisher's own forwarder must reach relay-global state on `relayExec_`:

```
[Pub]  publisherFwd fires onEmpty / forwardChanged / newGroupRequested
        └─ LocalForwarderCallback              # owns tlForwarders_ removal
           #   removeOnEmpty=false publish-initiated, true subscribe-initiated
             └─ CrossExecForwarderCallback ⇢⇢▶ [Relay]
[Relay]           └─ WeakRelayForwarderCallback ──▶ MoqxRelay::onEmptyImpl / forwardChangedImpl / newGroupRequestedImpl
```

### Local forwarder → publisher

Built by `buildLocalToPublisherCallbacks` (subscribe and fan-out paths). A subscriber thread's
`localFwd` is a channel subscriber of `publisherFwd`; when it empties or its forwarding state
changes, the publisher on `[Pub]` must react:

```
[Sub]  localFwd fires onEmpty / forwardChanged / newGroupRequested
        └─ LocalForwarderCallback              # owns localReg removal (removeOnEmpty=true)
             └─ CrossExecForwarderCallback ⇢⇢▶ [Pub]
[Pub]           └─ ChannelForwarderCallback:
                     onEmpty           → channelSub.detach(subscriberExec)
                                         # removeChannelSubscriberByExec on the publisher;
                                         # may cascade into the publisher chain above
                     forwardChanged    → requestUpdate(handle, forward)   # background coro
                     newGroupRequested → requestUpdate(handle, group)     # background coro
```

`ChannelForwarderCallback` reaches the publisher forwarder through a `ChannelSubscriber`, built
on `[Pub]` where a strong ref is legitimately in hand. It also holds the channel `handle_` (so it
can `requestUpdate` the publisher) and the `CrossExecFilter` ref; on `onEmpty` it posts the
filter's destruction back to `[Sub]` so FIFO ordering lets any in-flight object lambdas there run
before the filter is torn down.

### Setup window: `PendingForwarderCallback`

Between `join` and the real callback being installed, the forwarder can already fire
events. `PendingForwarderCallback` is installed first to capture them: it records the last
`forwardChanged`, the max `newGroupRequested` group, and whether `onEmpty` fired. After setup,
`replayPendingFowarderEvents` installs the real callback and replays the captured state; if
`onEmpty` was seen, the caller unwinds instead (`teardownLocalForwarderOnFailure`).

### Weak-ptr discipline

Ownership edges onto a forwarder are deliberately avoided with `weak_ptr`:

- A remote `ForwarderRef` holds `weak_ptr<forwarder>`, so the registry entry on `[Relay]` never
  keeps a publisher forwarder alive. It is only ever locked on the owner: `post()` locks inside
  the lambda it hands to `owner_->add()`, and `co_with()` locks inside a `Task` scheduled with
  `co_withExecutor(owner)`. The resulting strong ref lives and dies in a frame already running
  there, which is why liveness is judged on the owner and why the ref itself hands out nothing.
- `WeakRelayForwarderCallback` holds `weak_ptr<relay>` to break
  `registry → forwarder → callback → relay → registry`.
- `ChannelSubscriber` holds `weak_ptr<forwarder>` for the publisher forwarder, so the subscriber
  side never keeps it alive. It is built on `[Pub]` and keeps the weak private, so nothing off
  that thread can lock it; `detach()` checks it is running on the thread it was built on.

`CrossExecForwarderCallback` holds no reference to a forwarder at all. It reads
`fullTrackName()` on the calling thread — the forwarder's owner, where it is known alive — and
sends only the name, so nothing it dispatches can dereference or destroy a forwarder off its
executor.

### Teardown

```
source ends ──▶ TerminationFilter::publishDone()
  └─▶ onPublishDone(ftn)                                    # relay state cleanup
forwarder onPublishDone ──▶ LocalForwarderCallback ──▶ tlForwarders_->remove()   # identity-checked
```

Identity-checked removal makes teardown order-independent: a terminated forwarder can never
evict a newer one that has claimed the same track name. Subscriber-path local forwarders use
`removeOnEmpty=true`, so they also vacate when their last subscriber leaves; the publisher's
forwarder uses `removeOnEmpty=false` (see [Publish](#publish)).
