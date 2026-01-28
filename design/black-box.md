# Relay Provider Abstraction

## Overview

Design an abstraction layer for MoQRelay that provides publisher/subscriber connections across a fleet of relay nodes. When the local namespace tree doesn't have a publisher, this abstraction can engage other relay nodes to establish subscriptions.

## Current Architecture

**Note**: claude is looking at the moxygen POC relay code.  We'll be starting fresh here - the
important bits are the Provider interface and general application flows.

MoQRelay maintains a Namespace tree that tracks:
- Publishers who have sent PUBLISH_NAMESPACE for namespaces
- Publishers who have sent PUBLISH for specific tracks
- Subscribers with SUBSCRIBE_NAMESPACE on namespace prefixes

When a SUBSCRIBE arrives, MoQRelay calls `findSession()` to locate the upstream session. If not found locally, the subscribe fails.

## Design Principles

### Ownership Split

- **MoQRelay** owns:
  - `subscriptions_` map - tracks active subscriptions and their forwarders
  - `MoQForwarder` lifecycle - fanout to subscribers, onEmpty/forwardChanged callbacks
  - **Namespace tree** - the local namespace/track registry
  - Cache integration
  - Session-to-executor thread safety (wrapping consumers)
  - Implements `RelayProvider` for local resolution

- **Remote Providers** (UpstreamProvider, MeshProvider) own:
  - Outbound connections to remote relays/peers
  - Routing logic for selecting which remote to use
  - Delegating inbound requests (from remote sessions) back to MoQRelay via callback

### Bidirectional Sessions

Remote providers manage bidirectional sessions. When a provider connects to an upstream or peer:
- **Outbound**: MoQRelay calls `provider->subscribe(...)` to resolve remotely
- **Inbound**: The remote session may send PUBLISH, SUBSCRIBE, PUBLISH_NAMESPACE to us - these are delegated back to MoQRelay via the callback

### Symmetry

- **subscribe**: MoQRelay passes a consumer (forwarder) IN, provider publishes TO it
- **publish**: Provider returns a consumer, MoQRelay/publisher publishes TO it

This allows providers to be protocol-agnostic - the returned consumer could wrap MOQT, a custom mesh protocol, or any other transport.

### Fan-out vs Fallback

Operations have different semantics for local vs remote:

| Operation | Pattern | Rationale |
|-----------|---------|-----------|
| `subscribe` | **Fallback**: try local, then remote | Find ONE source for the track |
| `subscribeNamespace` | **Fan-out**: local AND remote | Want ALL namespace publications |
| `publish` | **Fan-out**: local AND remote | Reach ALL subscribers (local + remote) |
| `publishNamespace` | **Fan-out**: local AND remote | Make discoverable everywhere |

For fan-out publish, the MoQForwarder already handles multiple consumers - the remote provider's consumer is added as another output alongside local subscribers.

### Why Local Lives in MoQRelay

MoQRelay owns the namespace tree rather than having a separate LocalProvider because:

1. **Callback target**: Remote providers delegate inbound requests to MoQRelay - it's where they resolve
2. **Always present**: Even local-only mode needs the namespace tree; remote provider is optional
3. **Avoids circularity**: If local was a provider, its callback would be... MoQRelay anyway
4. **Simplicity**: MoQRelay IS the local relay - it's where connections terminate

## Proposed Abstraction

### Core Interface: `RelayProvider`

Providers implement the existing `Publisher` and `Subscriber` interfaces. This keeps them protocol-agnostic and allows direct composition.

```cpp
// RelayProvider combines Publisher + Subscriber interfaces
// Providers act as both:
// - Publisher: accepting subscriptions, delivering objects
// - Subscriber: accepting namespace publications, accepting publishes

class RelayProvider : public Publisher, public Subscriber {
 public:
  virtual ~RelayProvider() = default;

  // Inherits from Publisher:
  //   - subscribe(SubscribeRequest, TrackConsumer) -> SubscribeResult
  //   - fetch(Fetch, FetchConsumer) -> FetchResult
  //   - subscribeNamespace(SubscribeNamespace) -> SubscribeNamespaceResult
  //   - trackStatusRequest(TrackStatusRequest) -> TrackStatusResult
  //
  // Inherits from Subscriber:
  //   - publishNamespace(PublishNamespace, PublishNamespaceCallback) -> PublishNamespaceResult
  //   - publish(PublishRequest, SubscriptionHandle) -> PublishResult
};
```

### Callback Pattern

Remote providers are constructed with a callback reference to a `RelayProvider` (typically MoQRelay). When the provider's remote session receives inbound requests, it delegates them to the callback:

```cpp
class UpstreamProvider : public RelayProvider {
 public:
  // Callback is where inbound requests from upstream are delegated
  explicit UpstreamProvider(RelayProvider& callback, std::string upstreamUri);

 private:
  RelayProvider& callback_;  // typically MoQRelay
  // ...
};
```

This enables bidirectional communication:
- **Outbound**: MoQRelay -> Provider -> remote session
- **Inbound**: remote session -> Provider -> callback (MoQRelay)

**Redirect handling**: Redirect is a protocol-level concern. When a provider (e.g., MeshProvider) knows content is elsewhere, it returns a `RequestError` with `REDIRECT` code and an optional location field (similar to GOAWAY). The receiver can then follow the redirect or pass it back towards the client.

### MoQRelay Integration

MoQRelay implements `RelayProvider` for local resolution and uses fan-out or fallback depending on the operation:

```cpp
class MoQRelay : public RelayProvider {
 public:
  void setRemoteProvider(std::shared_ptr<RelayProvider> provider);

  // === subscribe: FALLBACK (try local, then remote) ===

  folly::coro::Task<SubscribeResult> subscribe(
      SubscribeRequest sub,
      std::shared_ptr<TrackConsumer> downstreamConsumer) override {

    auto& ftn = sub.fullTrackName;

    // Dedup: add to existing forwarder if present
    auto it = subscriptions_.find(ftn);
    if (it != subscriptions_.end()) {
      it->second.forwarder->addSubscriber(downstreamConsumer, ...);
      co_return it->second.handle;
    }

    auto forwarder = std::make_shared<MoQForwarder>(ftn, this);
    forwarder->addSubscriber(downstreamConsumer, ...);

    // Try local first
    auto session = findNamespaceSession(ftn.trackNamespace);
    if (session) {
      auto result = co_await session->subscribe(sub, forwarder);
      if (result.hasValue()) {
        subscriptions_.emplace(ftn, RelaySubscription{forwarder, result.value()});
        co_return makeSubscribeOk(...);
      }
      // Local failed - fall through to remote
    }

    // Try remote
    if (remoteProvider_) {
      auto result = co_await remoteProvider_->subscribe(sub, forwarder);
      if (result.hasValue()) {
        subscriptions_.emplace(ftn, RelaySubscription{forwarder, result.value()});
        co_return makeSubscribeOk(...);
      }
      co_return folly::makeUnexpected(result.error());
    }

    co_return folly::makeUnexpected(SubscribeError{...});
  }

  // === publish: FAN-OUT (local AND remote) ===

  PublishResult publish(
      PublishRequest pub,
      std::shared_ptr<SubscriptionHandle> handle) override {

    auto forwarder = getOrCreateForwarder(pub.fullTrackName);

    // Add remote consumer to forwarder (if remote has subscribers)
    if (remoteProvider_) {
      auto remoteResult = remoteProvider_->publish(pub, handle);
      if (remoteResult.hasValue()) {
        forwarder->addSubscriber(remoteResult.value().consumer, ...);
      }
    }

    // Return forwarder - publisher writes to it, fans out to local + remote
    if (forwarder->hasSubscribers()) {
      return PublishSuccess{forwarder, makePublishOkTask()};
    }

    return folly::makeUnexpected(PublishError{...});
  }

  // === publishNamespace: FAN-OUT (local AND remote) ===

  PublishNamespaceResult publishNamespace(
      PublishNamespace pub, ...) override {

    // Always store locally
    namespaceRoot_.add(pub.trackNamespace, session);
    notifySubscribeNamespaceSubscribers(pub.trackNamespace);

    // Also propagate to remote
    if (remoteProvider_) {
      remoteProvider_->publishNamespace(pub, ...);
    }

    return PublishNamespaceOk{...};
  }

  // === subscribeNamespace: FAN-OUT (local AND remote) ===

  SubscribeNamespaceResult subscribeNamespace(
      SubscribeNamespace sub) override {

    // Register locally - will receive local namespace publications
    namespaceRoot_.addSubscriber(sub.trackNamespacePrefix, subscriber);

    // Also register with remote - will receive remote namespace publications
    if (remoteProvider_) {
      remoteProvider_->subscribeNamespace(sub);
    }

    return SubscribeNamespaceOk{...};
  }

 private:
  std::shared_ptr<RelayProvider> remoteProvider_;
  NamespaceNode namespaceRoot_;
  folly::F14FastMap<FullTrackName, RelaySubscription> subscriptions_;
  std::unique_ptr<MoQCache> cache_;
};
```

Note: MoQRelay implements `RelayProvider`, so remote providers can use it as their callback target for inbound requests.

## Concrete Implementations

### 1. UpstreamProvider (Default Upstream)

Connects to a single upstream relay/origin for all unresolved requests. Takes a callback for handling inbound requests from the upstream session.

```cpp
class UpstreamProvider : public RelayProvider {
 public:
  UpstreamProvider(RelayProvider& callback, std::string upstreamUri)
      : callback_(callback), upstreamUri_(std::move(upstreamUri)) {}

  // === Outbound: Subscribe (called by MoQRelay) ===
  // Connects to upstream, sends SUBSCRIBE, wires response to consumer
  folly::coro::Task<SubscribeResult> subscribe(
      SubscribeRequest sub,
      std::shared_ptr<TrackConsumer> consumer) override {
    auto session = co_await getOrCreateSession();
    co_return co_await session->subscribe(sub, consumer);
  }

  // === Outbound: Publish (called by MoQRelay) ===
  // Returns a consumer that wraps the upstream MOQT session
  PublishResult publish(
      PublishRequest pub,
      std::shared_ptr<SubscriptionHandle> handle) override {
    auto session = getOrCreateSessionSync();
    return session->publish(pub, handle);
  }

 private:
  folly::coro::Task<std::shared_ptr<MoQSession>> getOrCreateSession() {
    if (!upstreamSession_) {
      upstreamSession_ = co_await connectToUpstream(upstreamUri_);
      // Wire inbound requests from upstream to callback
      upstreamSession_->setPublisher(callback_);   // inbound SUBSCRIBEs go to callback
      upstreamSession_->setSubscriber(callback_);  // inbound PUBLISHes go to callback
    }
    co_return upstreamSession_;
  }

  RelayProvider& callback_;  // where inbound requests are delegated (MoQRelay)
  std::string upstreamUri_;
  std::shared_ptr<MoQSession> upstreamSession_;
};
```

When the upstream session receives an inbound PUBLISH (e.g., upstream pushing content to us), it's delegated to `callback_.publish(...)`, which routes to MoQRelay's local subscribers.

### 2. MeshProvider (Example)

Example of a provider that routes to a mesh of peers using a custom protocol.

```cpp
class MeshProvider : public RelayProvider {
 public:
  explicit MeshProvider(RelayProvider& callback);

  folly::coro::Task<SubscribeResult> subscribe(
      SubscribeRequest sub,
      std::shared_ptr<TrackConsumer> consumer) override {
    // Look up route for namespace
    // Select peer based on weights
    // Forward subscribe to peer
    // Return result
  }

  PublishResult publish(
      PublishRequest pub,
      std::shared_ptr<SubscriptionHandle> handle) override {
    // Look up route for namespace
    // Select peer based on weights
    // Forward publish to peer
    // Return consumer that will publish to peer
  }

 private:
  RelayProvider& callback_;  // inbound requests from peers go here
  // Routing table, peer connections, etc.
};
```

### 3. ChainedProvider (Loop-based)

Chains multiple providers together, trying each in order until one succeeds. Child providers should be constructed with the appropriate callback before being added to the chain.

```cpp
struct ProviderEntry {
  std::shared_ptr<RelayProvider> provider;
  bool fallThrough;  // continue to next provider on "not found" errors?
};

class ChainedProvider : public RelayProvider {
 public:
  explicit ChainedProvider(std::vector<ProviderEntry> providers)
      : providers_(std::move(providers)) {}

  folly::coro::Task<SubscribeResult> subscribe(
      SubscribeRequest sub,
      std::shared_ptr<TrackConsumer> consumer) override {
    SubscribeError lastError{sub.requestID, SubscribeErrorCode::TRACK_NOT_EXIST, "No providers"};

    for (auto& entry : providers_) {
      auto result = co_await entry.provider->subscribe(sub, consumer);
      if (result.hasValue()) {
        co_return result;  // Success - done
      }

      lastError = result.error();

      // Continue to next provider if fallThrough and error is "not found" type
      if (!entry.fallThrough || !isFallThroughError(lastError)) {
        co_return folly::makeUnexpected(lastError);
      }
    }
    co_return folly::makeUnexpected(lastError);
  }

  // Similar loop for publish, fetch, publishNamespace, etc.

 private:
  bool isFallThroughError(const SubscribeError& err) {
    // Fall through on "not found" errors, stop on hard errors or redirects
    return err.errorCode == SubscribeErrorCode::TRACK_NOT_EXIST;
  }

  std::vector<ProviderEntry> providers_;
};
```

**Benefits of loop-based approach:**
- Providers are simpler - they implement standard Publisher/Subscriber without chaining awareness
- Chaining logic is centralized in one place
- `fallThrough` makes behavior explicit per-provider
- Easy to reason about: "try each until success, or stop on hard error/redirect"

Usage:
```cpp
// Providers are constructed with MoQRelay as callback, then added to chain
auto upstream = std::make_shared<UpstreamProvider>(relay, "moqt://origin:4433");

auto chained = std::make_shared<ChainedProvider>(std::vector<ProviderEntry>{
    {upstream, false}  // try upstream, stop here
});

relay.setRemoteProvider(chained);
```

### Typical Configurations

```cpp
// Local-only relay (current behavior, no remote provider)
auto relay = std::make_shared<MoQRelay>();

// Local + single upstream origin
auto relay = std::make_shared<MoQRelay>();
auto upstream = std::make_shared<UpstreamProvider>(*relay, "moqt://origin:4433");
relay->setRemoteProvider(upstream);

// Local + mesh routing (example - routes configured out-of-band)
auto relay = std::make_shared<MoQRelay>();
auto mesh = std::make_shared<MeshProvider>(*relay);
// ... configure peers and routes ...
relay->setRemoteProvider(mesh);

// Local + chained providers (try upstream1, then upstream2)
auto relay = std::make_shared<MoQRelay>();
auto upstream1 = std::make_shared<UpstreamProvider>(*relay, "moqt://origin1:4433");
auto upstream2 = std::make_shared<UpstreamProvider>(*relay, "moqt://origin2:4433");
auto chained = std::make_shared<ChainedProvider>(std::vector<ProviderEntry>{
    {upstream1, true},   // try first, fall through on miss
    {upstream2, false}   // try second, stop here
});
relay->setRemoteProvider(chained);
```

## Control Message Flow

### SUBSCRIBE: Fallback (local THEN remote)

**Outbound:**
1. Client -> Relay: SUBSCRIBE(track)
2. Relay checks existing subscriptions (dedup)
3. Relay creates forwarder, adds client as subscriber
4. Relay tries local namespace tree first
5. If not found locally, Relay -> RemoteProvider: subscribe(track, forwarder)
6. On Success: objects flow from source -> forwarder -> client
7. Relay -> Client: SUBSCRIBE_OK or SUBSCRIBE_ERROR

**Inbound (via Provider):**
1. Remote -> Provider: SUBSCRIBE(track)
2. Provider -> callback (MoQRelay): subscribe(track, consumer)
3. Relay tries local, then its remote provider (same fallback logic)
4. Relay -> Provider -> Remote: SUBSCRIBE_OK + objects

### PUBLISH: Fan-out (local AND remote)

**Outbound:**
1. Publisher -> Relay: PUBLISH(track)
2. Relay gets/creates forwarder for track
3. Relay -> RemoteProvider: publish(track) to get remote consumer
4. Relay adds remote consumer to forwarder
5. Relay returns forwarder to publisher
6. Publisher writes -> forwarder fans out to local + remote subscribers

**Inbound (via Provider):**
1. Remote -> Provider: PUBLISH(track)
2. Provider -> callback (MoQRelay): publish(track, handle)
3. Relay returns forwarder (with local + any other remote subscribers)
4. Remote objects flow through forwarder to all subscribers

### PUBLISH_NAMESPACE: Fan-out (local AND remote)

1. Publisher -> Relay: PUBLISH_NAMESPACE(namespace)
2. Relay stores in local namespace tree
3. Relay notifies local SUBSCRIBE_NAMESPACE subscribers
4. Relay -> RemoteProvider: publishNamespace(namespace) to propagate
5. Relay -> Publisher: PUBLISH_NAMESPACE_OK

### SUBSCRIBE_NAMESPACE: Fan-out (local AND remote)

1. Subscriber -> Relay: SUBSCRIBE_NAMESPACE(prefix)
2. Relay registers locally, sends existing local namespace publications
3. Relay -> RemoteProvider: subscribeNamespace(prefix)
4. Future PUBLISH_NAMESPACEs from local OR remote are forwarded to subscriber
5. Relay -> Subscriber: SUBSCRIBE_NAMESPACE_OK

### FETCH: Fallback via MissHandler chain

1. Client -> Relay: FETCH(track, range)
2. Relay builds MissHandler chain: LocalPublisher -> RemoteProvider -> NotFound
3. Relay calls `cache->fetch(fetch, consumer, missHandler)`
4. Cache serves hits; misses flow through the chain
5. Relay -> Client: FETCH_OK + objects

**MissHandler integration** (see `miss-handler.md`): `Publisher::fetch()` and `MissHandler::fetch()` have the same signature. MoQRelay wraps local publishers and remote providers as MissHandlers:

```cpp
folly::coro::Task<FetchResult> MoQRelay::fetch(Fetch f, FetchConsumer consumer) {
  // Build chain: local -> remote -> not found
  auto missHandler = std::make_shared<NotFoundMissHandler>();

  if (remoteProvider_) {
    missHandler = std::make_shared<PublisherMissHandler>(remoteProvider_, missHandler);
  }
  if (auto local = findNamespaceSession(f.fullTrackName.trackNamespace)) {
    missHandler = std::make_shared<PublisherMissHandler>(local, missHandler);
  }

  co_return co_await cache_->fetch(f, consumer, missHandler);
}
```

### TRACK_STATUS: Fallback (local THEN remote)

1. Client -> Relay: TRACK_STATUS_REQUEST(track)
2. Relay checks local knowledge
3. If not found, Relay -> RemoteProvider: trackStatus(track)
4. Relay -> Client: TRACK_STATUS response

## Open Questions

### Ownership & Lifecycle

1. **Callback lifetime**: Providers hold a reference to the callback (MoQRelay). Need to ensure MoQRelay outlives its providers, or use weak references.

2. **Thread safety**: MoQRelay and providers need executor affinity. Should provider interface include executor context, or is it implicit?

### Failure & Recovery

3. **Connection pooling**: How should providers (UpstreamProvider, MeshProvider) manage connections? Persistent? On-demand? Reconnection policy?

4. **Failure handling**: What happens if remote connection fails mid-stream?
   - Notify forwarder subscribers via subscribeDone?
   - Automatic failover to next provider in chain?
   - Re-resolve and reconnect?

### Protocol Behavior

5. ~~**Namespace propagation**~~: Resolved - PUBLISH_NAMESPACE and publish use fan-out (local AND remote), subscribe uses fallback (local THEN remote), subscribeNamespace uses fan-out.

6. **Hanging subscriptions**: When no publisher exists, subscriber may want to wait (with timeout) for one to appear. SUBSCRIBE message would include timeout param (relay may enforce max). MoQRelay parks the subscription, suspends the coro. When PUBLISH_NAMESPACE/PUBLISH arrives for matching track, wakes parked subs and wires them up. Remote parking delegated to provider.

## File Structure

```
relay/
├── MoQRelay.h/.cpp              (owns namespace tree, implements RelayProvider, delegates to remote)
├── RelayProvider.h              (interface - combines Publisher + Subscriber)
├── ChainedProvider.h/.cpp       (loop-based chaining)
├── UpstreamProvider.h/.cpp      (single upstream MOQT connection)
├── MeshProvider.h/.cpp          (example: mesh routing with custom protocol)
└── MoQForwarder.h/.cpp          (fan-out to multiple consumers)
```

## Migration Path

1. **Extract interface**: Create `RelayProvider.h` combining Publisher + Subscriber
2. **Implement Relay logic**: Implement RelayProvider, add setRemoteProvider(), keep namespace tree ownership
3. **Add UpstreamProvider**: Single-upstream implementation with callback pattern
4. **Add ChainedProvider**: Loop-based chaining with fallThrough config
5. **Add custom providers**: e.g., MeshProvider for mesh routing (as needed)
