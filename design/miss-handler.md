# MissHandler Interface Design Plan

## Overview

The `MissHandler` interface abstracts the mechanism for fetching objects not present in a local cache. This enables hierarchical cache composition - a cache can delegate to another cache, an upstream relay, or any other source that can fulfill fetch requests.

## Architecture

Miss handlers form a chain, passed per-fetch:

```
MoQCache::fetch(fetch, consumer, next)
                                   │
                                   ▼
                        DiskCache (MissHandler)
                                   │ next_
                                   ▼
                        RemoteCache (MissHandler)
                                   │ next_
                                   ▼
                        PublisherMissHandler
                                   │
                                   ▼
                                Origin
```

Or with a terminal handler:

```
MoQCache::fetch(fetch, consumer, next)
                                   │
                                   ▼
                        NotFoundMissHandler
                                   │
                                   ▼
                           (returns error)
```

## MissHandler Interface (Pure Virtual)

```cpp
class MissHandler {
 public:
  virtual ~MissHandler() = default;

  virtual folly::coro::Task<Publisher::FetchResult> fetch(
      Fetch fetch,
      std::shared_ptr<FetchConsumer> consumer) = 0;
};
```

## Terminal Handlers

### NotFoundMissHandler

Returns a FetchError indicating track not found:

```cpp
class NotFoundMissHandler : public MissHandler {
 public:
  folly::coro::Task<Publisher::FetchResult> fetch(
      Fetch fetch,
      std::shared_ptr<FetchConsumer>) override {
    return folly::coro::makeTask<Publisher::FetchResult>(
        folly::makeUnexpected(FetchError{
            fetch.requestID,
            FetchErrorCode::TRACK_NOT_FOUND,
            "not found"}));
  }
};
```

### NotExistsMissHandler

Returns FetchOk but signals that the requested objects don't exist:

```cpp
class NotExistsMissHandler : public MissHandler {
 public:
  folly::coro::Task<Publisher::FetchResult> fetch(
      Fetch fetch,
      std::shared_ptr<FetchConsumer> consumer) override {
    consumer->endOfFetch();
    return folly::coro::makeTask<Publisher::FetchResult>(
        std::make_shared<FetchHandle>(FetchOk{fetch.requestID, ...}));
  }
};
```

### UnavailableMissHandler (future, draft 16+)

Returns FetchOk with indication that the object range is unavailable:

```cpp
class UnavailableMissHandler : public MissHandler {
 public:
  folly::coro::Task<Publisher::FetchResult> fetch(
      Fetch fetch,
      std::shared_ptr<FetchConsumer> consumer) override {
    // Signal unavailable per draft 16
    // ...
  }
};
```

## PublisherMissHandler (Adapter)

Wraps a `Publisher` to use as a MissHandler:

```cpp
class PublisherMissHandler : public MissHandler {
 public:
  explicit PublisherMissHandler(std::shared_ptr<Publisher> publisher)
      : publisher_(std::move(publisher)) {}

  folly::coro::Task<Publisher::FetchResult> fetch(
      Fetch fetch,
      std::shared_ptr<FetchConsumer> consumer) override {
    return publisher_->fetch(std::move(fetch), std::move(consumer));
  }

 private:
  std::shared_ptr<Publisher> publisher_;
};
```

## MoQCache Changes

MoQCache implements MissHandler, but also accepts a MissHandler per-fetch (since different fetches may need different upstream chains):

**Before:**
```cpp
class MoQCache {
 public:
  folly::coro::Task<Publisher::FetchResult> fetch(
      Fetch fetch,
      std::shared_ptr<FetchConsumer> consumer,
      std::shared_ptr<Publisher> upstream);  // Publisher parameter
};
```

**After:**
```cpp
class MoQCache : public MissHandler {
 public:
  // Implements MissHandler - uses default miss handler (if set)
  folly::coro::Task<Publisher::FetchResult> fetch(
      Fetch fetch,
      std::shared_ptr<FetchConsumer> consumer) override;

  // Extended version - specify miss handler per-fetch
  folly::coro::Task<Publisher::FetchResult> fetch(
      Fetch fetch,
      std::shared_ptr<FetchConsumer> consumer,
      std::shared_ptr<MissHandler> next);

  // Optional: set default miss handler for the simple fetch() overload
  void setDefaultMissHandler(std::shared_ptr<MissHandler> handler);

 private:
  std::shared_ptr<MissHandler> defaultMissHandler_;
};
```

## Chain Setup Example

```cpp
// Terminal handlers
auto notFound = std::make_shared<NotFoundMissHandler>();
auto upstream = std::make_shared<PublisherMissHandler>(originPublisher);

// Shared cache instances
auto memoryCache = std::make_shared<MoQCache>();
auto diskCache = std::make_shared<DiskCache>();

// Different miss handlers per fetch scenario:

// Fetch from memory cache only, error if not found
co_await memoryCache->fetch(fetch1, consumer1, notFound);

// Fetch from memory cache, fallback to upstream
co_await memoryCache->fetch(fetch2, consumer2, upstream);

// Build a chain: memory -> disk -> upstream
// Disk cache wraps upstream, memory cache uses disk as its miss handler
auto diskWithUpstream = std::make_shared<DiskCache>(upstream);
co_await memoryCache->fetch(fetch3, consumer3, diskWithUpstream);

// Or equivalently, if DiskCache accepts miss handler per-fetch like MoQCache:
diskCache->setDefaultMissHandler(upstream);
co_await memoryCache->fetch(fetch4, consumer4, diskCache);
```

## Multi-Upstream Failover

A more advanced design might involve finding N potential upstreams and trying each in sequence, returning unavailable only if all fail:

```cpp
class FailoverMissHandler : public MissHandler {
 public:
  FailoverMissHandler(
      std::vector<std::shared_ptr<MissHandler>> upstreams,
      std::shared_ptr<MissHandler> next)
      : upstreams_(std::move(upstreams)), next_(std::move(next)) {}

  folly::coro::Task<Publisher::FetchResult> fetch(
      Fetch fetch,
      std::shared_ptr<FetchConsumer> consumer) override {
    for (auto& upstream : upstreams_) {
      auto result = co_await upstream->fetch(fetch, consumer);
      if (result.hasValue()) {
        co_return result;
      }
      // Log failure, try next upstream
    }
    // All upstreams failed, delegate to next_
    co_return co_await next_->fetch(std::move(fetch), std::move(consumer));
  }

 private:
  std::vector<std::shared_ptr<MissHandler>> upstreams_;
  std::shared_ptr<MissHandler> next_;
};
```

Usage:
```cpp
auto upstream1 = std::make_shared<PublisherMissHandler>(relay1);
auto upstream2 = std::make_shared<PublisherMissHandler>(relay2);
auto upstream3 = std::make_shared<PublisherMissHandler>(origin);
auto unavailable = std::make_shared<UnavailableMissHandler>();

auto failover = std::make_shared<FailoverMissHandler>(
    std::vector{upstream1, upstream2, upstream3},
    unavailable);  // terminal if all fail

co_await memoryCache->fetch(fetch, consumer, failover);
```

## Files to Modify

### 1. New File: `moxygen/relay/MissHandler.h`
- `MissHandler` pure virtual interface
- `NotFoundMissHandler` - returns FetchError
- `NotExistsMissHandler` - returns FetchOk + endOfFetch
- `PublisherMissHandler` - wraps Publisher

### 2. Modify: `moxygen/relay/MoQCache.h`
- Inherit from `MissHandler`
- Add `fetch()` overload that takes `std::shared_ptr<MissHandler> next`
- Change `upstream` param type from `Publisher` to `MissHandler`

### 3. Modify: `moxygen/relay/MoQCache.cpp`
- `fetchUpstream()` - change param type from `Publisher` to `MissHandler`
- Change `upstream->fetch(...)` calls to `next->fetch(...)`

### 4. Modify: `moxygen/relay/MoQRelay.cpp`
- Wrap upstream in `PublisherMissHandler` when calling `cache->fetch()`

## Open Question: Original vs Current Fetch Context

**Should MissHandler receive both the original fetch and the current (narrowed) miss range?**

**Use Case:** Cache requests objects 1-10. Objects 3-5 are cached. The miss handler sees calls for `fetch(1-2)` and `fetch(6-10)`. An upstream might prefer to fetch the entire 1-10 range in one request.

**Complications:**
- **Filters:** When fetch supports filters like `range(1-10), filter(id=2,4,6,8)`, a partial miss loses context about the original filter
- **Cache State:** Smart decisions may require knowing what's already cached

**Recommendation:** Start with the simple design (just the miss range). The miss handler can always fetch a larger range - FetchWriteback already caches all received objects. Revisit if filter support creates concrete problems.

## Design Notes: Coroutine Memory Usage

Long MissHandler chains mean nested coroutine frames, each with heap-allocated state that persists while waiting for the tail of the chain. For a chain like memory → disk → remote → origin, four coroutine frames are live simultaneously while waiting for the origin response.

**Perspective:** Non-coroutine async code (callbacks, futures) also requires heap-allocated state for resumption - the coroutine overhead is comparable, just more visible.

**Mitigations:**

1. **Minimize local variables in coroutines.** Each local variable in a coroutine that's live across a `co_await` is stored in the coroutine frame. Keep coroutine functions thin.

2. **Delegate synchronous work to helper functions.** Processing that doesn't need to suspend can use regular functions that allocate on the standard stack, which is freed when the function returns:

   ```cpp
   // Prefer this - processResult uses stack, freed before co_await
   folly::coro::Task<Result> fetch(...) {
     auto response = co_await upstream->fetch(...);
     co_return processResult(response);  // helper uses stack
   }

   // Avoid this - all locals live in coroutine frame
   folly::coro::Task<Result> fetch(...) {
     auto response = co_await upstream->fetch(...);
     std::vector<Object> objects;
     for (auto& item : response.items) {
       objects.push_back(transform(item));  // objects in frame
     }
     co_return Result{std::move(objects)};
   }
   ```

3. **Chain depth is typically small.** In practice, chains of 2-4 handlers are common. The memory overhead is proportional to chain depth, not data size.

## Verification

1. **Build:** Ensure MoQCache compiles with new inheritance
2. **Unit Tests:** Verify chain behavior - cache hit, cache miss delegation, terminal handler
3. **Integration:** Run relay with caching, verify fetch requests work through the chain
