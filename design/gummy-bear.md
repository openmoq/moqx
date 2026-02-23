# Multi-Publisher Deduplication Design Plan

## Problem Statement

When multiple publishers send the same track, the system needs to:
1. Forward data to subscribers without duplicating bytes
2. Handle concurrent `beginSubgroup` calls for the same (group, subgroup) from different publishers
3. Track metrics: bytes/objects served from each publisher, switch counts

## Architecture

```
Publisher A ──┐
Publisher B ──┼──> Deduplicator ──> Subscriber
Publisher C ──┘
```

Each publisher gets a unique `TrackConsumer` wrapper. The deduplicator maintains state and forwards deduplicated data to the single downstream consumer.

## Publisher Identification

Each publisher needs a unique identifier, but we want to avoid adding `PublisherId` parameters to every consumer method.

**Approach**: Add `currentPublisher()` to `SubgroupConsumer` base class, set when the chain begins:

```cpp
using PublisherId = uint64_t;

class SubgroupConsumer {
 public:
  // ... existing methods unchanged ...

  // Set by the deduplicator when beginSubgroup is called
  void setCurrentPublisher(PublisherId pub) { currentPublisher_ = pub; }
  PublisherId currentPublisher() const { return currentPublisher_; }

 private:
  PublisherId currentPublisher_{0};
};
```

The deduplicator sets `currentPublisher` on the `SubgroupConsumer` it returns to each publisher. Downstream consumers can query it when needed without changing method signatures.

## Object State Tracking

The deduplicator needs to know "how many bytes have we received for this object?" Rather than maintain its own cache, abstract this behind an interface that an existing cache (e.g., MoQCache) can implement:

```cpp
class ObjectStateTracker {
 public:
  struct ObjectState {
    uint64_t bytesReceived{0};
    bool complete{false};
  };

  // Query current state for an object
  virtual std::optional<ObjectState> getObjectState(
      uint64_t group, uint64_t subgroup, uint64_t object) = 0;

  // Update state after forwarding bytes
  virtual void updateObjectState(
      uint64_t group, uint64_t subgroup, uint64_t object,
      uint64_t bytesReceived, bool complete) = 0;
};
```

MoQCache already tracks cached objects - it can expose this interface. The deduplicator queries it, avoiding duplicate state.

## Deduplicator State

```cpp
class Deduplicator {
  // Delegate object tracking to existing cache
  ObjectStateTracker& objectTracker_;

  // Active downstream subgroups
  folly::F14FastMap<SubgroupIdentifier, std::shared_ptr<SubgroupConsumer>> activeSubgroups_;

  // Metrics per publisher
  struct PublisherMetrics {
    uint64_t bytesServed{0};
    uint64_t objectsServed{0};  // bump in beginObject/objectStream only
    uint64_t publishesFrom{0};  // bump each time we forward
  };
  folly::F14FastMap<PublisherId, PublisherMetrics> metrics_;

  std::shared_ptr<TrackConsumer> downstream_;
};
```

The cache handles both deduplication AND failover. No need to track "active" vs "shadow" publishers - any publisher can contribute bytes, duplicates get truncated.

## Byte-Level Deduplication

When a publisher sends object data:

1. **Check object cache**: Has this (g, s, obj) been seen?
2. **If new**: Forward all bytes, update cache
3. **If partial overlap**:
   - Calculate overlap with previously received bytes
   - Truncate duplicate prefix
   - Forward only new bytes (if any)
   - Update cache with new byte count
4. **If complete**: Return success but don't forward

```cpp
// Simplified deduplication logic (inside DeduplicatingSubgroupConsumer)
Result object(uint64_t objectID, Payload payload, ...) {
  auto pub = currentPublisher();
  auto state = dedup_.objectTracker_.getObjectState(group_, subgroup_, objectID);

  if (state && state->complete) {
    return success;  // already have it, skip
  }

  uint64_t existingBytes = state ? state->bytesReceived : 0;
  uint64_t payloadLen = payload->computeChainDataLength();

  if (payloadLen <= existingBytes) {
    return success;  // all bytes are duplicates
  }

  // Truncate duplicate prefix
  payload->trimStart(existingBytes);
  uint64_t newBytes = payloadLen - existingBytes;

  // Update tracker (cache will also store the data)
  dedup_.objectTracker_.updateObjectState(
      group_, subgroup_, objectID, payloadLen, /*complete=*/true);

  // Update metrics
  dedup_.metrics_[pub].bytesServed += newBytes;
  dedup_.metrics_[pub].publishesFrom++;

  // Forward remainder to downstream
  return downstream_->object(objectID, std::move(payload), ...);
}
```

## Subgroup Handling

When multiple publishers call `beginSubgroup` for the same (g, s):

- First caller creates the downstream subgroup
- Subsequent callers reuse the same downstream subgroup
- All publishers get a `SubgroupConsumer` that deduplicates through the shared cache

```cpp
Result beginSubgroup(uint64_t g, uint64_t s, Priority pri) {
  SubgroupIdentifier id{g, s};

  auto it = activeSubgroups_.find(id);
  if (it == activeSubgroups_.end()) {
    // First publisher - create downstream subgroup
    auto downstream = downstream_->beginSubgroup(g, s, pri);
    activeSubgroups_[id] = downstream.value();
  }

  // All publishers get a deduplicating consumer wrapping the shared downstream
  auto consumer = makeDeduplicatingConsumer(id, activeSubgroups_[id]);
  consumer->setCurrentPublisher(publisherId_);
  return consumer;
}
```

## Failover

When a publisher fails mid-subgroup, other publishers can continue seamlessly:

- The byte-level cache knows how many bytes have been received for each object
- New data from any publisher gets deduplicated against the cache
- No explicit "switching" needed - just keep deduplicating

## Without Deduplication

When deduplication is disabled/too expensive, the subscriber must handle multiple publishers directly.

**Problem**: Consumer APIs don't indicate the source.

```cpp
// Subscriber sees:
consumer->beginSubgroup(1, 0, pri);  // from pub A
consumer->beginSubgroup(1, 0, pri);  // from pub B - can't distinguish!
```

**Solution**: Use `currentPublisher()` from the returned `SubgroupConsumer`:

```cpp
// In subscriber's beginSubgroup:
auto subgroupConsumer = createSubgroupConsumer(g, s, pri);
auto pub = subgroupConsumer->currentPublisher();  // who is this from?

if (activeSubgroups_.contains({g, s})) {
  auto existingPub = activeSubgroups_[{g, s}];
  if (existingPub != pub) {
    // Different publisher for same subgroup - decide what to do
    // Return consumer that will error on subsequent calls
    return makeRejectingConsumer(pub, g, s);
  }
}
activeSubgroups_[{g, s}] = pub;
return subgroupConsumer;
```

The consumer can:
- Query `currentPublisher()` to identify the source
- Track which publisher owns each subgroup
- Return an error-generating consumer to the "losing" publisher

## Metrics

Per-publisher:
- `bytesServed`: Total bytes forwarded from this publisher
- `objectsServed`: Objects started (bump in `beginObject`/`objectStream` only)
- `publishesFrom`: Count of forwards from this publisher

This tells you which publishers are actually contributing data vs sending duplicates.

## Configuration

```cpp
struct DeduplicatorConfig {
  bool enabled{true};  // Can disable entirely
};
```

Object caching/eviction is handled by the underlying `ObjectStateTracker` implementation (e.g., MoQCache's existing LRU).

## Open Questions

1. **Streaming objects**: `beginObject`/`objectPayload` requires tracking partial object state across calls - more complex than complete objects.

2. **Content verification**: Should we hash content to verify same (g,s,obj) = same data?

3. **Priority conflicts**: If publishers send same subgroup with different priorities, which wins? (First caller?)

4. **Threading**: If publishers are on different threads, locking cost on the shared tracker?

5. **Subgroup cleanup**: When do we remove a subgroup from `activeSubgroups_`? When all publishers have ended/reset it?

## Verification

Unit tests:
- Two publishers sending identical objects → only forwarded once
- Two publishers with overlapping partial objects → bytes deduplicated correctly
- Publisher A fails mid-object, publisher B continues → no duplicate bytes
- `publishesFrom` correctly increments for the publisher whose data was forwarded

Integration tests:
- Multi-publisher scenario end-to-end
