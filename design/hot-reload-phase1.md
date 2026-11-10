# Hot reload, phase 1: add new listeners and services

## Context
moqx ignores `SIGHUP` today (`src/main.cpp:74-77`). Adding a listener or a service requires a restart. `design/hot-reloading.md` sets the direction: SIGHUP plus a runtime API.

The phases:
1. **This plan.** A reload adds listeners and services that are not already running. It can also bind an existing service to more listeners.
2. Later: tear down changed or removed listeners and services, and bring up their replacements. This reuses `drainListener` from `design/graceful-shutdown.md` (branch `graceful-shutdown-plan`).
3. Later: change a service in place, without teardown.

Decisions made:
- **Stacking:** the plan builds on PR #926 (listener/service binding) and on the peering stack #942–#945. The two stacks do not build on each other today: #926 predates `SessionParticipant`, and the two call `onSessionEnd` differently. Commit 1 needs a combined base. Either one stack rebases onto the other, or the reload work waits until both land.
- **Unsupported changes:** when the file contains changes phase 1 cannot apply, the reload applies the additions and reports each ignored change. It does not reject the whole reload. This differs from `design/hot-reloading.md` ("triggers an error"), so that doc gets updated.
- **New services on existing listeners** are allowed, except where the service adds a WebTransport exact path to an existing picoquic listener. Its `wtEndpoints` is fixed when the listener is constructed. In that case the service is not bound to that pico listener. Its other bindings apply, and the result reports what was dropped.
- **Triggers:** `SIGHUP`, and `POST /config/reload` on the admin server.
- **Naming:** the snapshot is `ServiceTable`, so it is not confused with `RouteCodec` / `ParticipantRouting`.
- **Relay threads:** keep the single relay pool and grow it, so each service still gets its own thread.

**Multiple config files** do not affect this plan. A reload runs the same loader as startup and diffs two *resolved* configs. When includes arrive, the loader changes and reload does not.

## Problems the design has to solve
1. **Routing state is shared without an owner.**
   - IO threads read `services_` and the listener matchers (`validateAuthority`, `onSessionEnd`).
   - The admin thread reads them too (`dumpState`, `purgeCache`, `aggregateTrackMetrics`).
   - The main thread reads them in the pico constructor (`exactPaths()`), and so do tests.
   - This works only because the table stays fixed after startup. Locks are ruled out by executor confinement.
2. **`onSessionEnd` matches the session's authority and path again** to find the session's service. After a reload adds a more specific service, a session ends on the wrong relay.
   - In the peering stack, `MoqxRelay::onSessionEnd` calls only `reciprocal_->onPeerClosed` (`MoqxRelay.cpp:1882-1887`).
   - So the visible result is a phantom downstream peer in `/state` and a stale `peerIdOf`. It is not a handle leak, and the `drainExecs` XCHECK does not fire.
3. **`StatsRegistry::collectors_` assumes registration is finished before the admin server serves** (`StatsRegistry.cpp:186,195`).
   - A listener added at runtime registers collectors from the main evb (`setStatsRegistry`) and from IO evbs (`QuicStatsCollector`, `PicoQuicStatsCollector`).
   - Meanwhile, `/metrics` and `/state` read the list from the admin thread.
4. **`EventBaseStatsCollector::create` replaces each evb's observer** (`EventBaseStatsCollector.cpp:13-17`). A new listener on the shared IO evbs would freeze the stats of the existing listeners.
5. **`MoqxPicoRelayServer::stop()` calls `context_->stop()`** (`MoqxPicoRelayServer.cpp:106`). Its destructor calls `stop()`. If a pico listener added at runtime fails to bind, dropping it stops peering for every service. Commit 1 of the graceful-shutdown plan makes the same fix.
6. **The relay thread pool is sized** to `services.size()` (`MoqxRelayContext.cpp:37`).
7. **A random `relay_id` and `cluster.hop_id` are generated on every resolve** when the config omits them (`ConfigResolver.cpp:1431,1436-1438`). A naive diff would report them as changed on every reload.
8. **Main-evb hops during teardown.**
   - After `loopForever` returns, the admin server keeps serving until `main.cpp:298`.
   - A hop queued on the main evb at that point either hangs the admin handler, or runs in `~EventBase` after `ioExecutor.reset()`.
   - Under the graceful-shutdown plan the main evb keeps looping during the drain, so a reload has to be refused then as well.
9. **`servers` is a local in `main`,** and `/config` returns the snapshot captured at startup.
10. **Existing services that omit `listeners` resolve to "every listener".** After a reload that adds a listener, their resolved list grows.

## Design

### 0. Prep fixes (own commits; startup behaves as before)
- **Pico `stop()`:** drop `context_->stop()`. `main` already calls it. This is shared with the graceful-shutdown plan; whichever lands first takes it.
- **`EventBaseStatsCollector::create`:** idempotent per evb. It returns the existing collector from `registry->evbCollector(evb)` when there is one.
- **`StatsRegistry` ownership:** the main evb owns `collectors_`, following the same publish pattern as the service table.
  - `registerCollector` posts the append to the main evb with `runImmediatelyOrRunInEventBaseThread`, so registrations from IO evbs do not block. After each change, the main evb publishes a copy of the list to the admin evb.
  - `aggregateAsync` reads the admin evb's copy. `/metrics` and `/state` stay on the admin evb, so they cannot get stuck behind a main loop that has stopped (problem 8).
  - At startup, `main` publishes the list once when the admin server starts.
  - Registrations that IO evbs post while `start()` runs are applied on the next main-loop turn. A scrape in that window can miss a new listener's counters for one turn.

### 1. Sessions remember their relay
- `MoqxRelaySession` (#926) gains a `std::shared_ptr<MoqxRelay>`, set in `validateAuthority`. The session's filters already hold the relay through `shared_from_this()`, so this does not extend its lifetime.
- `onSessionEnd` keeps the peering stack's `runOnExec(relay->getRelayExec(), … relay->onSessionEnd(sessionId))`, and drops the authority/path re-match.

### 2. `ServiceTable` snapshot in `MoqxRelayContext`
- New immutable `struct ServiceTable { F14FastMap<string, shared_ptr<const ServiceEntry>> services; F14NodeMap<string, ServiceMatcher> listenerMatchers; }`.
  - `ServiceEntry` is move-only since #942, because it holds a `unique_ptr<PeeringBackend>`.
  - `peering->start/stop/join` work through the const entry.
- **Owner, the main evb:** keeps `shared_ptr<const ServiceTable> table_`.
  - Every table contains every entry ever added, so the owner drops the last reference after `join()`.
  - The IO thread-locals die at `ioExecutor.reset()`, while `main` still holds the context.
- **Admin evb:** `AdminServer` exposes its single worker evb after `start()`.
  - **To check during implementation:** what proxygen `ScopedHTTPServer`/`HTTPServer` exposes. A fallback is to capture the evb on the server's start callback.
  - Admin starts after the servers, so the admin copy is published when the admin server starts, before any admin route runs.
- **Explicit `publish(ioEvbs, adminEvb)`:** sets a `folly::ThreadLocal<shared_ptr<const ServiceTable>>` on each IO evb and on the admin evb, with `runInEventBaseThreadAndWait`.
  - `main` calls it once before constructing any server, and `addServices` calls it on every reload.
  - It is safe because IO and admin paths do not block on the main evb. `initThreadStatsCollectors` and `bindAll` use the same pattern.
- **Readers:**
  - `validateAuthority` and `ListenerHandle::match` read the thread's copy.
  - `ListenerHandle::exactPaths()` is only called from server constructors on the main evb, so it reads `table_`.
  - `dumpState`, `purgeCache` and `aggregateTrackMetrics` read the admin evb's copy, so admin reads stay on the admin evb.
  - Tests get a `publishToCurrentThreadForTest()`, because `MoqxRelayContextTest` calls `validateAuthority` on the test thread.
- **Raw pointers stay valid in phase 1,** because phase 1 does not remove anything:
  - `SessionParticipant`'s `PeeringBackend*`;
  - the reciprocal handler;
  - the `const PeeringBackend*` copied by `dumpState`.
- **Relay threads:** `addServices` calls `relayThreadPool_->setNumThreads(n + k)`. The new evbs are the ones in `getAllEventBases()` that the context has not assigned yet.
  - Verified in folly: `getAllEventBases()` → `ensureMaxActiveThreads()` creates the threads, and the thread list is ordered by increasing id, so new evbs come last.
  - IO threads do not time out; only `CPUThreadPoolExecutor` calls `tryTimeoutThread()`.
  - Removal is for phase 2. `stopThreads(n)` stops the first n threads (`IOThreadPoolExecutor.cpp:326`), so it cannot target one evb. Phase 2 keeps an emptied thread on a free list, and may later add a folly API that stops a chosen thread.
- **`addServices(added, rebound)`, on the main evb:**
  1. Build every upstream verifier first (`makeUpstreamVerifier`). A service whose verifier throws is skipped before anything dials. `MoqtRelayBackend::startUpstream` can throw after earlier upstreams already started.
  2. Grow the pool. Create each relay, then call #942's `addService` helper, changed to return the entry instead of inserting it.
  3. Bind track stats (`trackStatsRegistry().bindAll`) with the saved IO execs and the `initTrackStats` flag.
  4. Call `peering->start(workerEvb_)` on each new backend. If one throws anyway, call `stop()` on it and do not publish the service. Its entry stays in the owner's list, so `drainExecs`/`join()` still cover it.
  5. Build the next table, with matchers rebuilt for each affected listener, and `publish`.
- The constructor shares these helpers. The zero-services and zero-listeners paths are unreachable, because the resolver rejects both (`ConfigResolver.cpp:1223,1322`). That avoids deferred init and any race on `workerEvb_`/`ready()`.

### 3. Config diff: new `src/config/ConfigDiff.{h,cpp}` (pure, unit-tested)
`ReloadPlan diff(const ResolvedConfig& running, const ResolvedConfig& next)` returns:
- **`addedListeners`** and **`addedServices`:** names that are new in `next`.
- **`reboundServices`:** existing services whose only change is a `listeners` list that is a strict superset of the old one. This is how problem 10 is handled.
- **`ignored`:** `{path, reason}` for each changed or removed listener or service, and for each changed global (`threads`, `relay_id`, `use_relay_thread`, `use_local_forwarders`, `mvfst_bpf_steering`, `cluster`, `admin`, `logging`).
  - A service with an omitted `listeners` and any other change is ignored as a whole. The result names it as "not bound to new listener X", so the operator knows why.
  - **Problem 7:** `ResolvedConfig` gains `relayIdGenerated` and `hopIdGenerated`. When `next` generated a value, the diff treats it as unchanged. The flags live in `ResolvedConfig`, not `Config`, so the serializer field-count assert is not affected.
- **`effective`:** `running` plus the additions and rebinds. This becomes the running config of record.

Then run two checks on `effective`:
- **Route conflicts:** factor #926's shared-listener duplicate (authority, path) check out of `ConfigResolver.cpp` into a function over resolved services. A new service that conflicts with the old version of an ignored service goes to `ignored`.
- **Pico paths:** an added or rebound service that adds an exact path to an existing picoquic listener loses that one listener from its `listeners`. The result reports it.

**Equality:** defaulted `operator==` on `ListenerConfig`, `ServiceConfig` and their members. TLS compares paths, plus `TlsMaterial` content. As a result, a rotated `.p12` counts as a changed listener, while a PEM file rotated in place does not. `docs/config.md` documents this.

### 4. `ConfigReloader`: new `src/reload/ConfigReloader.{h,cpp}` (main evb only)
- **Owns:**
  - the running `ResolvedConfig`;
  - a `ListenerSet`, a map from listener name to server, which replaces `main`'s `servers` vector;
  - the inputs `makeRelayServer` needs.
- **Loading:** factor `loadResolvedConfig(path, strict) -> Expected<ResolvedConfig, vector<string>>` out of `handleConfigSubcommand`. It uses the existing `loadConfig`, `resolveConfig` and the strict-warnings check, and returns errors instead of writing them to stderr. Startup and reload both call it. The name `loadConfig` is already taken (`Loader.h:19`).
- **`ReloadResult reload()`:**
  1. Refuse if `shuttingDown_` is set.
  2. Load and diff. If the diff does not add anything, report the ignored changes and return.
  3. `context->addServices(...)`. This comes before any server is constructed, because pico reads `exactPaths()` in its constructor.
  4. For each added listener, run `makeRelayServer` and then `start()` inside a try/catch. Both mvfst and pico throw on bind failure, and mvfst rolls itself back.
     - A failed listener goes to `errors`, is dropped, and is removed from every service's `listeners` in `effective`.
     - A service left without a live listener is reported as a warning. Its matcher entry for the dead listener is harmless.
  5. Swap the running config. Log a summary, plus a WARN for each ignored change and error.
- **Synchronous:** `reload()` does not suspend. Serialization comes from the main evb running it to completion. If it ever suspends, it needs an in-progress guard.
- **Teardown** iterates `ListenerSet`. The graceful-shutdown `ShutdownCoordinator` should drive the same `ListenerSet`, so listeners added at runtime also drain.

### 5. Triggers and shutdown guard
- **SIGHUP:** `SignalHandler` calls `reloader.reload()`. Keep `ignoreHangup()` at startup.
- **`POST /config/reload`:** new `src/admin/ReloadHandler.{h,cpp}`.
  - The handler checks an admin-evb `shuttingDown` flag before it hops to the main evb, and answers 503 if the flag is set.
  - Otherwise it runs `reload()` on the main evb and responds 200 with the `ReloadResult` JSON, or 400 when the load fails.
  - The trust model matches `POST /cache/purge`.
- **Shutdown:** before `terminateLoopSoon` (or `drainAll` under graceful shutdown), `main` sets `shuttingDown_` on the main evb and on the admin evb, with `runInEventBaseThreadAndWait`. After that, the admin evb does not start a hop to the main evb, so a hop cannot be stranded once the main loop stops.
- **`/config`:** reads an admin-evb copy of the running config. Each reload publishes the copy along with the service table. Update the header comment that says moqx does not support reload.

### 6. Docs
- `docs/config.md`: a "Reloading" section covering what applies, what is ignored, the pico restriction and TLS rotation.
- `design/hot-reloading.md`: the phase status and the ignore-and-warn behavior.
- One `changelog.d/added/` entry for the stack.

### 7. Notes for later phases (not built here)
- **Phase 3 upstream changes:** they map onto `PeeringBackend::executePeerAdminOp`. Today `MoqtRelayBackend` returns `Unsupported`. Admin peer ops will also make the running peers drift from `Config.upstreams`.
- **Phase 2 removal:** needs per-service stop → drain → join. The raw pointers listed in §2 then have to become owned or checked.

## Commits (on a combined #926 + #942–#945 base)
1. `pico: stop the per-listener stop() from stopping the shared context` (§0, shared with graceful shutdown).
2. `stats: make the registry and evb collectors safe to add to at runtime` (§0).
3. `relay: end each session on the relay it joined` (§1).
4. `relay: publish the service table as an immutable snapshot per executor` (§2, without `addServices`). Behavior does not change.
5. `relay: add services to a running context` (§2 `addServices`, grow the relay pool).
6. `config: diff a reloaded config against the running one` (§3, plus `loadResolvedConfig` and the factored route check).
7. `reload: add listeners and services on SIGHUP` (§4, §5 SIGHUP and the shutdown guard, `/config`).
8. `admin: POST /config/reload` (§5), plus docs and changelog (§6).

Critical files: `src/MoqxRelayContext.{h,cpp}`, `src/MoqxRelaySession.h`, `src/main.cpp`, `src/MoqxPicoRelayServer.cpp`, `src/stats/StatsRegistry.cpp`, `src/stats/EventBaseStatsCollector.cpp`, `src/config/ConfigResolver.cpp`, `src/config/ResolvedConfig.h`, `src/config/loader/ConfigInit.cpp`, `src/admin/ConfigHandler.{h,cpp}`, `src/MoqxServerFactory.h`.

## Verification
- **`ConfigDiffTest`:**
  - one case per category;
  - a service with omitted `listeners` rebinds to a new listener;
  - an omitted service with another change is reported as not bound;
  - a generated `relay_id`/`hop_id` is not reported;
  - a changed global is ignored while the additions still apply;
  - a new service that conflicts with the old version of an ignored service is skipped;
  - a pico exact path drops only that listener.
- **`MoqxRelayContextTest`:**
  - `addServices` routes new sessions on every IO thread;
  - after a more specific service is added, a peer session from before the reload ends on its original relay, so `/state` does not show a phantom downstream peer;
  - the admin readers see the new service;
  - a verifier failure skips the service without dialing;
  - `drainExecs`/`join()` cover added backends.
- **Stats:** a listener added at runtime does not freeze the existing listeners' evb stats, and `/metrics` stays consistent during the add. Run under ASAN.
- **`MoqxRelayServerTest` / harness:**
  - SIGHUP with a new listener and service, then a client connects on the new port while an existing subscription keeps flowing;
  - the same through `POST /config/reload`, checking the JSON;
  - a bind failure on a pico listener goes to `errors`, and the upstreams of existing services stay connected;
  - a reload during shutdown returns 503.
- **Commands:** `./scripts/dev/format.sh`, `./scripts/build.sh san`, `./scripts/test.sh san`, and `SECTIONS="8"` conformance.
