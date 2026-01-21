# Moxygen Vanilla API Wrapper Plan

## Overview
Create a C++20 standard library wrapper around moxygen's Publisher, Subscriber, and MoQConsumers interfaces in the `moxygen::vanilla` namespace, minimizing folly exposure in the public API.

## Seven-Phase Approach

### Phase 1: Migrate MoQFramer to std::optional
First commit: Replace `folly::Optional` with `std::optional` in MoQFramer.h and related files.

### Phase 2: Extract Core Types
Second commit: Split core types out of MoQFramer.h into a new `Types.h` that both moxygen and moxygen::vanilla can depend on.

### Phase 3: Replace IOBuf in Extensions
Third commit: Replace `std::unique_ptr<folly::IOBuf>` with `std::vector<uint8_t>` in Extension's arrayValue.

### Phase 4: Extract MoQPublishError
Fourth commit: Move MoQPublishError to its own file so vanilla can include it without MoQConsumers.h baggage.

### Phase 5: MoQExecutor SharedPtr
Fifth commit: Update MoQExecutor to use `folly::DefaultKeepAliveExecutor` and add `SharedPtr` type alias.

### Phase 6: Create vanilla Wrapper Interfaces
Sixth commit: Create the wrapper interfaces in `ti/experimental/moxygen/vanilla/`.

### Phase 7: Client, Server, and Adapters
Seventh commit: Create vanilla::Client, vanilla::Server, and bidirectional adapters.

---

## Phase 1: MoQFramer std::optional Migration

**Files to modify:**
- `ti/experimental/moxygen/MoQFramer.h`
- `ti/experimental/moxygen/MoQFramer.cpp`
- Any other files that use `folly::Optional` with these types

**Changes:**
1. Replace `#include <folly/Optional.h>` with `#include <optional>`
2. Replace `folly::Optional<T>` with `std::optional<T>`
3. Replace `folly::none` with `std::nullopt`
4. Update any `folly::Optional`-specific APIs

---

## Phase 2: Extract Core Types

**New file:** `ti/experimental/moxygen/Types.h`

Extract the following from MoQFramer.h into Types.h:

```cpp
// ti/experimental/moxygen/Types.h
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace moxygen {

// Basic type aliases
using Priority = uint8_t;
constexpr uint8_t kDefaultPriority = 128;

// Simple wrapper types
struct TrackAlias { uint64_t value{0}; /* operators... */ };
struct RequestID { uint64_t value{0}; /* operators... */ };
struct AbsoluteLocation { uint64_t group{0}; uint64_t object{0}; /* operators... */ };

// Enums (no folly deps)
enum class SessionCloseErrorCode : uint32_t { ... };
enum class RequestErrorCode : uint32_t { ... };
enum class SubscribeDoneStatusCode : uint32_t { ... };
enum class ResetStreamErrorCode : uint32_t { ... };
enum class ObjectStatus : uint64_t { ... };
enum class GroupOrder : uint8_t { ... };
enum class LocationType : uint8_t { ... };

// Error type aliases
using ErrorCode = SessionCloseErrorCode;
using SubscribeErrorCode = RequestErrorCode;
// ... other aliases

struct RequestError { RequestID requestID; RequestErrorCode errorCode; std::string reasonPhrase; };
using SubscribeError = RequestError;
using FetchError = RequestError;
// ... other aliases

// TrackNamespace and FullTrackName
struct TrackNamespace { std::vector<std::string> trackNamespace; /* methods... */ };
struct FullTrackName { TrackNamespace trackNamespace; std::string trackName; /* operators... */ };

// Message types (after std::optional migration)
struct SubscribeRequest { ... };
struct SubscribeOk { ... };
struct SubscribeDone { ... };
struct SubscribeUpdate { ... };
struct Fetch { ... };
struct FetchOk { ... };
struct Announce { ... };
struct AnnounceOk { ... };
struct PublishRequest { ... };
struct PublishOk { ... };
struct Goaway { ... };
// etc.

} // namespace moxygen
```

**Note:** MoQPublishError and ObjectPublishStatus stay in MoQConsumers.h.

**MoQFramer.h changes:**
- `#include <ti/experimental/moxygen/Types.h>`
- Remove extracted types
- Keep framing/parsing/writing logic and types specific to wire format

**Benefits:**
- Core types have no folly deps (after Phase 1)
- vanilla can include Types.h directly
- Cleaner separation of concerns

---

## Phase 3: Replace IOBuf in Extensions

**Files to modify:**
- `ti/experimental/moxygen/MoQFramer.h` (or `Types.h` after Phase 2)
- `ti/experimental/moxygen/MoQFramer.cpp`
- Any code that creates/reads Extension arrayValue

**Changes:**

```cpp
// Before
struct Extension {
  uint64_t type;
  uint64_t intValue;
  std::unique_ptr<folly::IOBuf> arrayValue;
  // ...
};

// After
struct Extension {
  uint64_t type;
  uint64_t intValue;
  std::vector<uint8_t> arrayValue;

  Extension() noexcept : Extension(0, 0) {}
  Extension(uint64_t type, uint64_t intValue) noexcept
      : type(type), intValue(intValue) {}
  Extension(uint64_t type, std::vector<uint8_t> arrayValue) noexcept
      : type(type), intValue(0), arrayValue(std::move(arrayValue)) {}

  bool isOddType() const { return type & 0x1; }

  bool operator==(const Extension& other) const noexcept {
    if (type != other.type) return false;
    if (isOddType()) {
      return arrayValue == other.arrayValue;
    }
    return intValue == other.intValue;
  }
};
```

**Parser changes:**
- Where IOBuf was created wrapping incoming buffer, now copy into vector
- `cursor.clone(length)` → `std::vector<uint8_t>(length)` + `cursor.pull()`

**Writer changes:**
- Where IOBuf was written, now write vector data directly

**Benefits:**
- Extension is now completely folly-free
- Simpler copy/move semantics (no clone() needed)
- Extensions are typically small, copy cost is negligible

---

## Phase 4: Extract MoQPublishError

**New file:** `ti/experimental/moxygen/MoQPublishError.h`

Move `MoQPublishError` and `ObjectPublishStatus` from `MoQConsumers.h` to their own header so vanilla can include them without pulling in all of MoQConsumers.h.

```cpp
// ti/experimental/moxygen/MoQPublishError.h
#pragma once

#include <cstdint>
#include <string>

namespace moxygen {

enum class ObjectPublishStatus : uint8_t {
  DONE,
  IN_PROGRESS,
  BLOCKED
};

struct MoQPublishError {
  enum class Code : uint8_t {
    CANCELLED,
    RESET,
    BLOCKED,
    // ... other codes
  };

  Code code;
  std::string message;
};

} // namespace moxygen
```

**Update MoQConsumers.h:**
```cpp
#include <ti/experimental/moxygen/MoQPublishError.h>
// Remove the extracted types, just include the new header
```

**Benefits:**
- vanilla can `#include <ti/experimental/moxygen/MoQPublishError.h>` directly
- No need to include MoQConsumers.h which has folly dependencies

---

## Phase 5: MoQExecutor SharedPtr

**Files to modify:**
- `ti/experimental/moxygen/events/MoQExecutor.h`
- `ti/experimental/moxygen/events/MoQFollyExecutorImpl.h/.cpp`
- `ti/experimental/moxygen/events/MoQLibevExecutorImpl.h/.cpp`
- Any code using `std::shared_ptr<MoQExecutor>` → update to `MoQExecutor::SharedPtr`

**Changes to MoQExecutor:**

```cpp
#pragma once

#include <folly/DefaultKeepAliveExecutor.h>
#include <quic/common/events/QuicEventBase.h>
#include <chrono>

namespace moxygen {

class MoQExecutor : public folly::DefaultKeepAliveExecutor {
public:
  // KeepAlive-based shared pointer type
  using SharedPtr = folly::Executor::KeepAlive<MoQExecutor>;

  SharedPtr getSharedPtr() {
    return folly::getKeepAliveToken(*this);
  }

  ~MoQExecutor() override {
    joinKeepAlive();  // Blocks until all KeepAlives released
  }

  template <
      typename T,
      typename = std::enable_if_t<std::is_base_of_v<MoQExecutor, T>>>
  T* getTypedExecutor() {
    return dynamic_cast<T*>(this);
  }

  // Timeout scheduling methods
  virtual void scheduleTimeout(
      quic::QuicTimerCallback* callback,
      std::chrono::milliseconds timeout) = 0;
};

} // namespace moxygen
```

**Update implementations:**

MoQFollyExecutorImpl and MoQLibevExecutorImpl now inherit from the updated MoQExecutor which uses DefaultKeepAliveExecutor.

**Migration:**

Replace throughout codebase:
- `std::shared_ptr<MoQExecutor>` → `MoQExecutor::SharedPtr`
- `std::make_shared<MoQFollyExecutorImpl>(...)` → `executor->getSharedPtr()`

**Usage pattern:**

```cpp
// Create executor (owned by caller)
auto exec = std::make_unique<MoQFollyExecutorImpl>(&evb);

// Get KeepAlive token for passing to other components
MoQExecutor::SharedPtr execPtr = exec->getSharedPtr();

// Pass to vanilla::Client
vanilla::Client client(execPtr, url);

// Executor destructor blocks until all KeepAlives are released
```

### Phase 4 (Commit 4): MoQExecutor SharedPtr Implementation Order
1. Update `MoQExecutor.h` to extend `folly::DefaultKeepAliveExecutor`
2. Add `SharedPtr` type alias and `getSharedPtr()` method
3. Add `joinKeepAlive()` call in destructor
4. Update `MoQFollyExecutorImpl` and `MoQLibevExecutorImpl`
5. Migrate existing `std::shared_ptr<MoQExecutor>` usages to `MoQExecutor::SharedPtr`
6. Build and test

---

## Phase 6: vanilla Wrapper Interfaces

## File Structure

```
ti/experimental/moxygen/vanilla/
├── BUCK
├── Expected.h           # Custom Expected<T, E> + Unit type
├── Payload.h            # Payload interface + VectorPayload + SharedPayload (no folly)
├── Payload.cpp          # Implementations of VectorPayload, SharedPayload
├── IOBufPayload.h       # IOBufPayload (includes folly)
├── IOBufPayload.cpp     # IOBufPayload implementation + payload conversion
├── Callbacks.h          # All callback interface definitions
├── Publisher.h          # Publisher interface with callbacks
├── Subscriber.h         # Subscriber interface with callbacks
└── MoQConsumers.h       # TrackConsumer, SubgroupConsumer, FetchConsumer
```

---

## 1. Expected.h - Custom Expected Type

```cpp
namespace moxygen::vanilla {

// Unit type for void returns in Expected
struct Unit {};

template <typename E>
class Unexpected {
  E error_;
public:
  explicit Unexpected(E e) : error_(std::move(e)) {}
  const E& value() const& { return error_; }
  E& value() & { return error_; }
  E&& value() && { return std::move(error_); }
};

template <typename E>
Unexpected<E> makeUnexpected(E e) { return Unexpected<E>(std::move(e)); }

template <typename T, typename E>
class Expected {
  std::variant<T, Unexpected<E>> storage_;
public:
  Expected(T value) : storage_(std::move(value)) {}
  Expected(Unexpected<E> error) : storage_(std::move(error)) {}

  bool hasValue() const { return std::holds_alternative<T>(storage_); }
  bool hasError() const { return !hasValue(); }

  T& value() & { return std::get<T>(storage_); }
  const T& value() const& { return std::get<T>(storage_); }
  T&& value() && { return std::get<T>(std::move(storage_)); }

  E& error() & { return std::get<Unexpected<E>>(storage_).value(); }
  const E& error() const& { return std::get<Unexpected<E>>(storage_).value(); }

  explicit operator bool() const { return hasValue(); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }
};

} // namespace moxygen::vanilla
```

**Note:** If `folly::Expected` is acceptable for vanilla consumers, the adaptation surface
is significantly reduced - adapters can pass Expected values through without conversion.
Consider making vanilla::Expected a thin wrapper or alias for folly::Expected if full
folly independence is not required.

---

## 2. Payload.h/.cpp - Payload Interface and Implementations

```cpp
namespace moxygen::vanilla {

// Type enum for efficient type checking (avoids dynamic_cast)
enum class PayloadType { Vector, Shared, IOBuf };

// Abstract interface - completely folly-free
class Payload {
public:
  virtual ~Payload() = default;

  // Type identification
  virtual PayloadType type() const = 0;

  // Read access via spans (one span per buffer segment)
  virtual const std::vector<std::span<const uint8_t>>& spans() const = 0;
  virtual size_t totalSize() const = 0;
  virtual bool empty() const = 0;

  // Clone
  virtual std::unique_ptr<Payload> clone() const = 0;
};

// ---- Concrete implementations ----

// Backed by std::vector<uint8_t> (pure std, no folly)
class VectorPayload : public Payload {
  std::vector<uint8_t> data_;
  mutable std::vector<std::span<const uint8_t>> spans_;
public:
  explicit VectorPayload(std::vector<uint8_t> data);
  VectorPayload(const void* data, size_t size);

  PayloadType type() const override { return PayloadType::Vector; }
  const std::vector<std::span<const uint8_t>>& spans() const override;
  size_t totalSize() const override;
  bool empty() const override;
  std::unique_ptr<Payload> clone() const override;

  std::vector<uint8_t> releaseData() { return std::move(data_); }
};

// Backed by shared_ptr<uint8_t[]> (for shared ownership scenarios)
class SharedPayload : public Payload {
  std::shared_ptr<uint8_t[]> data_;
  size_t size_;
  mutable std::vector<std::span<const uint8_t>> spans_;
public:
  SharedPayload(std::shared_ptr<uint8_t[]> data, size_t size);

  PayloadType type() const override { return PayloadType::Shared; }
  const std::vector<std::span<const uint8_t>>& spans() const override;
  size_t totalSize() const override;
  bool empty() const override;
  std::unique_ptr<Payload> clone() const override;

  // Release ownership for zero-copy conversion
  std::pair<std::shared_ptr<uint8_t[]>, size_t> releaseData() {
    return {std::move(data_), size_};
  }
};

} // namespace moxygen::vanilla
```

**IOBufPayload.h/.cpp** (separate file, links against folly):

```cpp
namespace moxygen::vanilla {

// Backed by folly::IOBuf - zero-copy wrapper
class IOBufPayload : public Payload {
  std::unique_ptr<folly::IOBuf> buf_;
  mutable std::vector<std::span<const uint8_t>> spans_;
public:
  explicit IOBufPayload(std::unique_ptr<folly::IOBuf> buf);

  PayloadType type() const override { return PayloadType::IOBuf; }
  const std::vector<std::span<const uint8_t>>& spans() const override;
  size_t totalSize() const override;
  bool empty() const override;
  std::unique_ptr<Payload> clone() const override;

  // IOBuf-specific access for adapter layer
  folly::IOBuf* iobuf() const { return buf_.get(); }
  std::unique_ptr<folly::IOBuf> releaseIOBuf() { return std::move(buf_); }
};

// Payload conversion utilities (used by adapters) - destructive, takes ownership
std::unique_ptr<folly::IOBuf> payloadToIOBuf(std::unique_ptr<Payload> payload);
std::unique_ptr<Payload> iobufToPayload(std::unique_ptr<folly::IOBuf> buf);

} // namespace moxygen::vanilla
```

**Payload conversion implementation (in IOBufPayload.cpp):**

```cpp
std::unique_ptr<folly::IOBuf> payloadToIOBuf(std::unique_ptr<Payload> payload) {
  switch (payload->type()) {
    case PayloadType::IOBuf: {
      // Zero-copy: move the IOBuf out
      return static_cast<IOBufPayload*>(payload.get())->releaseIOBuf();
    }
    case PayloadType::Vector: {
      // Zero-copy: take ownership of vector's buffer
      auto* vecPayload = static_cast<VectorPayload*>(payload.get());
      auto vec = vecPayload->releaseData();
      if (vec.empty()) {
        return folly::IOBuf::create(0);
      }
      // Transfer vector ownership to IOBuf via custom deleter
      auto* data = vec.data();
      auto size = vec.size();
      auto* vecPtr = new std::vector<uint8_t>(std::move(vec));
      return folly::IOBuf::takeOwnership(
          data, size,
          [](void*, void* userData) {
            delete static_cast<std::vector<uint8_t>*>(userData);
          },
          vecPtr);
    }
    case PayloadType::Shared: {
      // Zero-copy: share ownership via shared_ptr custom deleter
      auto* sharedPayload = static_cast<SharedPayload*>(payload.get());
      auto [data, size] = sharedPayload->releaseData();
      if (!data || size == 0) {
        return folly::IOBuf::create(0);
      }
      // Keep shared_ptr alive via custom deleter
      auto* sharedPtr = new std::shared_ptr<uint8_t[]>(std::move(data));
      return folly::IOBuf::takeOwnership(
          sharedPtr->get(), size,
          [](void*, void* userData) {
            delete static_cast<std::shared_ptr<uint8_t[]>*>(userData);
          },
          sharedPtr);
    }
  }
  // Should never reach here
  return folly::IOBuf::create(0);
}

std::unique_ptr<Payload> iobufToPayload(std::unique_ptr<folly::IOBuf> buf) {
  return std::make_unique<IOBufPayload>(std::move(buf));
}
```

---

## 3. Callbacks.h - Callback Interfaces

```cpp
#pragma once

#include <ti/experimental/moxygen/Types.h>
#include <ti/experimental/moxygen/MoQPublishError.h>  // Standalone, no MoQConsumers baggage
#include <memory>
#include <optional>

namespace moxygen::vanilla {

// Forward declarations
class SubscriptionHandle;
class FetchHandle;
class SubscribeAnnouncesHandle;
class AnnounceHandle;
class TrackConsumer;
class FetchConsumer;

// Note: Types like SubscribeError, TrackAlias, etc. resolve to moxygen::
// via normal unqualified lookup since vanilla is nested in moxygen.

// --- Publisher Callbacks ---

class SubscribeCallback {
public:
  virtual ~SubscribeCallback() = default;
  virtual void onSubscribeOk(std::shared_ptr<SubscriptionHandle> handle) = 0;
  virtual void onSubscribeError(SubscribeError error) = 0;
};

class FetchCallback {
public:
  virtual ~FetchCallback() = default;
  virtual void onFetchOk(std::shared_ptr<FetchHandle> handle) = 0;
  virtual void onFetchError(FetchError error) = 0;
};

class TrackStatusCallback {
public:
  virtual ~TrackStatusCallback() = default;
  virtual void onTrackStatusOk(TrackStatusOk ok) = 0;
  virtual void onTrackStatusError(TrackStatusError error) = 0;
};

class SubscribeAnnouncesCallback {
public:
  virtual ~SubscribeAnnouncesCallback() = default;
  virtual void onSubscribeAnnouncesOk(std::shared_ptr<SubscribeAnnouncesHandle> handle) = 0;
  virtual void onSubscribeAnnouncesError(SubscribeAnnouncesError error) = 0;
};

class SubscribeUpdateCallback {
public:
  virtual ~SubscribeUpdateCallback() = default;
  virtual void onSubscribeUpdateOk(SubscribeUpdateOk ok) = 0;
  virtual void onSubscribeUpdateError(SubscribeUpdateError error) = 0;
};

// --- Subscriber Callbacks ---

class AnnounceCallback {
public:
  virtual ~AnnounceCallback() = default;
  virtual void onAnnounceOk(std::shared_ptr<AnnounceHandle> handle) = 0;
  virtual void onAnnounceError(AnnounceError error) = 0;
};

class PublishReplyCallback {
public:
  virtual ~PublishReplyCallback() = default;
  virtual void onPublishOk(PublishOk ok) = 0;
  virtual void onPublishError(PublishError error) = 0;
};

// --- Backpressure Callbacks ---

class ReadyCallback {
public:
  virtual ~ReadyCallback() = default;
  virtual void onReady(uint64_t availableBytes) = 0;
  virtual void onError(MoQPublishError error) = 0;
};

class StreamCreditCallback {
public:
  virtual ~StreamCreditCallback() = default;
  virtual void onCreditAvailable() = 0;
  virtual void onError(MoQPublishError error) = 0;
};

// --- Delivery Callback ---

class DeliveryCallback {
public:
  virtual ~DeliveryCallback() = default;
  virtual void onDelivered(
      std::optional<TrackAlias> maybeTrackAlias,
      uint64_t groupId, uint64_t subgroupId, uint64_t objectId) = 0;
  virtual void onDeliveryCancelled(
      std::optional<TrackAlias> maybeTrackAlias,
      uint64_t groupId, uint64_t subgroupId, uint64_t objectId) = 0;
};

} // namespace moxygen::vanilla
```

---

## 4. Publisher.h - Publisher Interface

```cpp
#pragma once

#include <ti/experimental/moxygen/Types.h>
#include <ti/experimental/moxygen/vanilla/Callbacks.h>
#include <memory>

namespace moxygen::vanilla {

class SubscriptionHandle {
public:
  virtual ~SubscriptionHandle() = default;
  virtual void unsubscribe() = 0;
  virtual void subscribeUpdate(
      SubscribeUpdate update,
      std::shared_ptr<SubscribeUpdateCallback> callback) = 0;
  virtual const SubscribeOk& subscribeOk() const = 0;
};

class FetchHandle {
public:
  virtual ~FetchHandle() = default;
  virtual void fetchCancel() = 0;
  virtual const FetchOk& fetchOk() const = 0;
};

class SubscribeAnnouncesHandle {
public:
  virtual ~SubscribeAnnouncesHandle() = default;
  virtual void unsubscribeAnnounces() = 0;
  virtual const SubscribeAnnouncesOk& subscribeAnnouncesOk() const = 0;
};

class Publisher {
public:
  virtual ~Publisher() = default;

  virtual void trackStatus(
      TrackStatus request,
      std::shared_ptr<TrackStatusCallback> callback) = 0;

  virtual void subscribe(
      SubscribeRequest request,
      std::shared_ptr<TrackConsumer> consumer,
      std::shared_ptr<SubscribeCallback> callback) = 0;

  virtual void fetch(
      Fetch request,
      std::shared_ptr<FetchConsumer> consumer,
      std::shared_ptr<FetchCallback> callback) = 0;

  virtual void subscribeAnnounces(
      SubscribeAnnounces request,
      std::shared_ptr<SubscribeAnnouncesCallback> callback) = 0;

  virtual void goaway(Goaway goaway) = 0;
};

} // namespace moxygen::vanilla
```

---

## 5. Subscriber.h - Subscriber Interface

```cpp
#pragma once

#include <ti/experimental/moxygen/Types.h>
#include <ti/experimental/moxygen/vanilla/Callbacks.h>
#include <memory>
#include <optional>

namespace moxygen::vanilla {

class AnnounceHandle {
public:
  virtual ~AnnounceHandle() = default;
  virtual void unannounce() = 0;
  virtual const AnnounceOk& announceOk() const = 0;
};

class AnnounceNotifier {
public:
  virtual ~AnnounceNotifier() = default;
  virtual void announceCancel(AnnounceErrorCode code, std::string reason) = 0;
};

struct PublishResult {
  std::shared_ptr<TrackConsumer> consumer;  // nullptr on error
  std::optional<PublishError> error;
  std::shared_ptr<PublishReplyCallback> replyCallback;
};

class Subscriber {
public:
  virtual ~Subscriber() = default;

  virtual void announce(
      Announce request,
      std::shared_ptr<AnnounceNotifier> notifier,
      std::shared_ptr<AnnounceCallback> callback) = 0;

  virtual PublishResult publish(
      PublishRequest request,
      std::shared_ptr<SubscriptionHandle> handle = nullptr) = 0;

  virtual void goaway(Goaway goaway) = 0;
};

} // namespace moxygen::vanilla
```

---

## 6. MoQConsumers.h - Consumer Interfaces

```cpp
namespace moxygen::vanilla {

class SubgroupConsumer {
public:
  virtual ~SubgroupConsumer() = default;

  virtual Expected<Unit, MoQPublishError> object(
      uint64_t objectID,
      std::unique_ptr<Payload> payload,
      Extensions extensions = {},
      bool finSubgroup = false) = 0;

  virtual Expected<Unit, MoQPublishError> objectNotExists(
      uint64_t objectID,
      bool finSubgroup = false) = 0;

  virtual void checkpoint() = 0;

  virtual Expected<Unit, MoQPublishError> beginObject(
      uint64_t objectID,
      uint64_t length,
      std::unique_ptr<Payload> initialPayload,
      Extensions extensions = {}) = 0;

  virtual Expected<ObjectPublishStatus, MoQPublishError> objectPayload(
      std::unique_ptr<Payload> payload,
      bool finSubgroup = false) = 0;

  virtual Expected<Unit, MoQPublishError> endOfGroup(uint64_t objectID) = 0;
  virtual Expected<Unit, MoQPublishError> endOfTrackAndGroup(uint64_t objectID) = 0;
  virtual Expected<Unit, MoQPublishError> endOfSubgroup() = 0;
  virtual void reset(ResetStreamErrorCode error) = 0;

  // Backpressure - callback-based
  virtual void awaitReadyToConsume(std::shared_ptr<ReadyCallback> callback) = 0;
};

class TrackConsumer {
public:
  virtual ~TrackConsumer() = default;

  virtual Expected<Unit, MoQPublishError> setTrackAlias(TrackAlias alias) = 0;

  virtual Expected<std::shared_ptr<SubgroupConsumer>, MoQPublishError>
  beginSubgroup(uint64_t groupID, uint64_t subgroupID, Priority priority) = 0;

  virtual void awaitStreamCredit(std::shared_ptr<StreamCreditCallback> callback) = 0;

  virtual Expected<Unit, MoQPublishError> objectStream(
      const ObjectHeader& header,
      std::unique_ptr<Payload> payload) = 0;

  virtual Expected<Unit, MoQPublishError> datagram(
      const ObjectHeader& header,
      std::unique_ptr<Payload> payload) = 0;

  virtual Expected<Unit, MoQPublishError> groupNotExists(
      uint64_t groupID, uint64_t subgroup, Priority pri) = 0;

  virtual Expected<Unit, MoQPublishError> subscribeDone(SubscribeDone done) = 0;

  virtual void setDeliveryCallback(std::shared_ptr<DeliveryCallback> callback) = 0;
};

class FetchConsumer {
public:
  virtual ~FetchConsumer() = default;

  virtual Expected<Unit, MoQPublishError> object(
      uint64_t groupID, uint64_t subgroupID, uint64_t objectID,
      std::unique_ptr<Payload> payload, Extensions extensions = {}, bool finFetch = false) = 0;

  virtual Expected<Unit, MoQPublishError> objectNotExists(
      uint64_t groupID, uint64_t subgroupID, uint64_t objectID,
      bool finFetch = false) = 0;

  virtual Expected<Unit, MoQPublishError> groupNotExists(
      uint64_t groupID, uint64_t subgroupID, bool finFetch = false) = 0;

  virtual void checkpoint() = 0;

  virtual Expected<Unit, MoQPublishError> beginObject(
      uint64_t groupID, uint64_t subgroupID, uint64_t objectID,
      uint64_t length, std::unique_ptr<Payload> initialPayload, Extensions extensions = {}) = 0;

  virtual Expected<ObjectPublishStatus, MoQPublishError> objectPayload(
      std::unique_ptr<Payload> payload, bool finSubgroup = false) = 0;

  virtual Expected<Unit, MoQPublishError> endOfGroup(
      uint64_t groupID, uint64_t subgroupID, uint64_t objectID,
      bool finFetch = false) = 0;

  virtual Expected<Unit, MoQPublishError> endOfTrackAndGroup(
      uint64_t groupID, uint64_t subgroupID, uint64_t objectID) = 0;

  virtual Expected<Unit, MoQPublishError> endOfFetch() = 0;
  virtual void reset(ResetStreamErrorCode error) = 0;

  virtual void awaitReadyToConsume(std::shared_ptr<ReadyCallback> callback) = 0;
};

} // namespace moxygen::vanilla
```

---

## Implementation Order

### Phase 1 (Commit 1): MoQFramer std::optional Migration
1. Update MoQFramer.h - replace folly::Optional with std::optional
2. Update MoQFramer.cpp - update any .value() or folly::none usage
3. Update all consumers of these types throughout moxygen
4. Build and test

### Phase 2 (Commit 2): Extract Core Types
1. Create `ti/experimental/moxygen/Types.h`
2. Move core types from MoQFramer.h to Types.h
3. Update MoQFramer.h to include Types.h
4. Update BUCK file with new target
5. Build and test

### Phase 3 (Commit 3): Replace IOBuf in Extensions
1. Change `Extension::arrayValue` from `std::unique_ptr<folly::IOBuf>` to `std::vector<uint8_t>`
2. Update Extension constructors and operators
3. Update parser to copy into vector instead of wrapping IOBuf
4. Update writer to write vector data directly
5. Update all code that reads/writes Extension arrayValue
6. Build and test

### Phase 6 (Commit 6): vanilla Wrapper Interfaces
1. **Expected.h** - Custom Expected<T, E> using std::variant + Unit type
2. **Payload.h/.cpp** - Payload interface + VectorPayload + SharedPayload (no folly deps)
3. **IOBufPayload.h/.cpp** - IOBufPayload (links folly)
4. **Callbacks.h** - All callback interface definitions
5. **MoQConsumers.h** - SubgroupConsumer, TrackConsumer, FetchConsumer
6. **Publisher.h** - Publisher, SubscriptionHandle, FetchHandle, SubscribeAnnouncesHandle
7. **Subscriber.h** - Subscriber, AnnounceHandle
8. **BUCK** - Build file (separate targets for folly and non-folly code)

---

## Key Design Decisions

1. **Namespace**: `moxygen::vanilla` - nested in moxygen, so core types (TrackAlias, SubscribeRequest, etc.) resolve automatically without explicit `moxygen::` prefix

2. **Payload**: Abstract interface with concrete implementations
   - `Payload` base class is completely folly-free
   - `VectorPayload`, `SharedPayload` - pure std implementations
   - `IOBufPayload` - folly-backed, in separate files
   - All expose data via `std::vector<std::span<const uint8_t>>`

3. **Extensions**: Use `moxygen::Extensions` directly (folly-free after Phase 3)

4. **ObjectHeader**: Keep using `moxygen::ObjectHeader` as-is

5. **Message types**: Use directly from moxygen:: namespace (no re-exports)
   - MoQPublishError extracted to standalone file in Phase 4
   - Other types extracted to moxygen/Types.h in Phase 2

6. **Callbacks**: Single callback interface per operation with success/error methods

7. **Backpressure**: Callback-based via `ReadyCallback` and `StreamCreditCallback`

8. **Executor handling** (for future adapter): Adapter classes accept executor at construction, nullptr allowed (inline callback invocation)

---

## Files Summary

### Phase 1 - Files to Modify
- `ti/experimental/moxygen/MoQFramer.h`
- `ti/experimental/moxygen/MoQFramer.cpp`
- Other files using folly::Optional with these types

### Phase 2 - Files to Create/Modify
**New:**
- `ti/experimental/moxygen/Types.h` - Core types extracted from MoQFramer.h

**Modify:**
- `ti/experimental/moxygen/MoQFramer.h` - Include Types.h, remove extracted types
- `ti/experimental/moxygen/BUCK` - Add Types target

### Phase 3 - Files to Modify
- `ti/experimental/moxygen/Types.h` (or MoQFramer.h if Extension not yet moved)
- `ti/experimental/moxygen/MoQFramer.cpp` - Parser/writer changes
- Any code creating/reading Extension arrayValue

### Phase 6 - New Files to Create
```
ti/experimental/moxygen/vanilla/
├── BUCK
├── Expected.h         # Custom Expected<T, E> + Unit type
├── Payload.h          # Payload interface + VectorPayload + SharedPayload
├── Payload.cpp
├── IOBufPayload.h     # IOBufPayload (folly dep)
├── IOBufPayload.cpp
├── Callbacks.h
├── MoQConsumers.h
├── Publisher.h
└── Subscriber.h
```

---

## Phase 7: Client, Server, and Adapters

### Overview

Create `vanilla::Client` that wraps MoQClient to provide callback-based session establishment. Client inherits from `vanilla::Publisher` and `vanilla::Subscriber` so callers can use `client->subscribe()` directly. Uses `MoQExecutor::SharedPtr` for executor lifetime management.

### Threading Model

- `connect()`, `subscribe()`, `fetch()`, `announce()`, etc. can be called from **any thread**
- Operations are queued to the executor thread for execution
- All callbacks are invoked on the **executor thread**
- Thread-safe: callers don't need external synchronization
- Adapters handle the thread boundary internally

### New Files

```
ti/experimental/moxygen/vanilla/
├── Client.h                    # vanilla::Client interface, ConnectCallback
├── Client.cpp                  # Client implementation
├── Server.h                    # vanilla::Server interface, SessionCallback
├── Server.cpp                  # Server implementation
└── adapters/
    ├── BUCK
    ├── PublisherAdapters.h     # MoxygenPublisherWrapper, VanillaPublisherWrapper
    ├── PublisherAdapters.cpp
    ├── SubscriberAdapters.h    # MoxygenSubscriberWrapper, VanillaSubscriberWrapper
    ├── SubscriberAdapters.cpp
    ├── ConsumerAdapters.h      # Track/Subgroup/Fetch consumer wrappers
    ├── ConsumerAdapters.cpp
    ├── HandleAdapters.h        # SubscriptionHandle, FetchHandle, etc. wrappers
    └── HandleAdapters.cpp
```

**Adapter naming convention:**
- `MoxygenPublisherWrapper` - wraps `moxygen::Publisher`, implements `vanilla::Publisher`
- `VanillaPublisherWrapper` - wraps `vanilla::Publisher`, implements `moxygen::Publisher`
- Same pattern for Subscriber, TrackConsumer, SubgroupConsumer, FetchConsumer, handles

### Client.h

```cpp
#pragma once

#include <ti/experimental/moxygen/events/MoQExecutor.h>
#include <ti/experimental/moxygen/vanilla/Publisher.h>
#include <ti/experimental/moxygen/vanilla/Subscriber.h>
#include <memory>
#include <string>

namespace moxygen::vanilla {

class ConnectCallback {
public:
  virtual ~ConnectCallback() = default;
  virtual void onConnected() = 0;
  virtual void onConnectError(SessionCloseErrorCode error, std::string reason) = 0;
};

// Client inherits Publisher and Subscriber for direct method access
class Client : public vanilla::Publisher, public vanilla::Subscriber {
public:
  Client(MoQExecutor::SharedPtr exec, const std::string& url);
  ~Client();

  // Non-copyable, movable
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) noexcept;
  Client& operator=(Client&&) noexcept;

  // Connect to server. Can be called from any thread.
  // Callbacks invoked on executor thread.
  // publishHandler/subscribeHandler are for incoming requests (can be nullptr)
  void connect(
      std::shared_ptr<vanilla::Publisher> publishHandler,
      std::shared_ptr<vanilla::Subscriber> subscribeHandler,
      std::shared_ptr<ConnectCallback> callback,
      std::chrono::milliseconds connectTimeout = std::chrono::seconds(5),
      std::chrono::milliseconds setupTimeout = std::chrono::seconds(5));

  // Publisher interface - inherited, callable directly on client
  void trackStatus(TrackStatus request,
      std::shared_ptr<TrackStatusCallback> callback) override;
  void subscribe(SubscribeRequest request,
      std::shared_ptr<vanilla::TrackConsumer> consumer,
      std::shared_ptr<SubscribeCallback> callback) override;
  void fetch(Fetch request,
      std::shared_ptr<vanilla::FetchConsumer> consumer,
      std::shared_ptr<FetchCallback> callback) override;
  void subscribeAnnounces(SubscribeAnnounces request,
      std::shared_ptr<SubscribeAnnouncesCallback> callback) override;
  void goaway(Goaway goaway) override;  // From Publisher

  // Subscriber interface - inherited, callable directly on client
  void announce(Announce request,
      std::shared_ptr<AnnounceNotifier> notifier,
      std::shared_ptr<AnnounceCallback> callback) override;
  PublishResult publish(PublishRequest request,
      std::shared_ptr<SubscriptionHandle> handle) override;
  // goaway already declared from Publisher

  // Graceful shutdown
  void close(SessionCloseErrorCode error = SessionCloseErrorCode::NO_ERROR);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace moxygen::vanilla
```

### Implementation Strategy

`Client::Impl` contains:
- `MoQClient` for connection establishment
- `MoxygenPublisherWrapper` adapting session to vanilla::Publisher
- `MoxygenSubscriberWrapper` adapting session to vanilla::Subscriber

Client methods delegate to the wrappers after connection.

### Adapter Examples

**MoxygenPublisherWrapper - wraps moxygen::Publisher, implements vanilla::Publisher:**

Used by vanilla::Client to expose moxygen session as vanilla interface.

```cpp
// In adapters/PublisherAdapters.cpp

class MoxygenPublisherWrapper : public vanilla::Publisher {
  std::shared_ptr<moxygen::Publisher> moxygenPublisher_;
  MoQExecutor::SharedPtr exec_;

public:
  MoxygenPublisherWrapper(
      std::shared_ptr<moxygen::Publisher> pub,
      MoQExecutor::SharedPtr exec)
      : moxygenPublisher_(std::move(pub)), exec_(std::move(exec)) {}

  void subscribe(
      SubscribeRequest request,
      std::shared_ptr<vanilla::TrackConsumer> consumer,
      std::shared_ptr<vanilla::SubscribeCallback> callback) override {

    // Wrap vanilla consumer for moxygen API
    auto moxygenConsumer = std::make_shared<VanillaTrackConsumerWrapper>(consumer);

    auto publisher = moxygenPublisher_;

    // Launch detached coroutine
    folly::coro::co_invoke(
        [publisher, request = std::move(request), moxygenConsumer, callback]()
            -> folly::coro::Task<void> {
          try {
            auto result = co_await publisher->subscribe(request, moxygenConsumer);

            if (result.hasValue()) {
              auto vanillaHandle = std::make_shared<MoxygenSubscriptionHandleWrapper>(
                  std::move(result.value()));
              callback->onSubscribeOk(std::move(vanillaHandle));
            } else {
              callback->onSubscribeError(result.error());
            }
          } catch (const folly::OperationCancelled&) {
            callback->onSubscribeError(SubscribeError{
                request.requestID,
                SubscribeErrorCode::INTERNAL_ERROR,
                "Operation cancelled"});
          } catch (const std::exception& e) {
            callback->onSubscribeError(SubscribeError{
                request.requestID,
                SubscribeErrorCode::INTERNAL_ERROR,
                e.what()});
          }
        })
        .scheduleOn(exec_.get())
        .start();
  }

  // Similar pattern for fetch(), trackStatus(), subscribeAnnounces(), goaway()
};
```

**VanillaPublisherWrapper - wraps vanilla::Publisher, implements moxygen::Publisher:**

Used by servers to expose vanilla handlers to moxygen session.

```cpp
// In adapters/PublisherAdapters.cpp

class VanillaPublisherWrapper : public moxygen::Publisher {
  std::shared_ptr<vanilla::Publisher> vanillaPublisher_;
  MoQExecutor::SharedPtr exec_;

public:
  VanillaPublisherWrapper(
      std::shared_ptr<vanilla::Publisher> pub,
      MoQExecutor::SharedPtr exec)
      : vanillaPublisher_(std::move(pub)), exec_(std::move(exec)) {}

  folly::coro::Task<SubscribeResult> subscribe(
      SubscribeRequest request,
      std::shared_ptr<moxygen::TrackConsumer> consumer) override {

    // Wrap moxygen consumer for vanilla API
    auto vanillaConsumer = std::make_shared<MoxygenTrackConsumerWrapper>(consumer);

    // Promise/future to bridge callback → coroutine
    auto [promise, future] = folly::coro::makePromiseContract<SubscribeResult>();

    // Callback that fulfills the promise
    class BridgeCallback : public vanilla::SubscribeCallback {
      folly::coro::Promise<SubscribeResult> promise_;
    public:
      explicit BridgeCallback(folly::coro::Promise<SubscribeResult> p)
          : promise_(std::move(p)) {}

      void onSubscribeOk(
          std::shared_ptr<vanilla::SubscriptionHandle> handle) override {
        auto moxygenHandle = std::make_shared<VanillaSubscriptionHandleWrapper>(handle);
        promise_.setValue(SubscribeResult{std::move(moxygenHandle)});
      }

      void onSubscribeError(SubscribeError error) override {
        promise_.setValue(folly::makeUnexpected(error));
      }
    };

    // Call vanilla API (callback will be invoked on executor thread)
    vanillaPublisher_->subscribe(
        request,
        vanillaConsumer,
        std::make_shared<BridgeCallback>(std::move(promise)));

    // Suspend until callback fires
    co_return co_await std::move(future);
  }

  // Similar pattern for fetch(), trackStatus(), etc.
};
```

**MoxygenSubscriptionHandleWrapper:**

```cpp
class MoxygenSubscriptionHandleWrapper : public vanilla::SubscriptionHandle {
  std::shared_ptr<moxygen::Publisher::SubscriptionHandle> moxygenHandle_;
  MoQExecutor::SharedPtr exec_;

public:
  explicit MoxygenSubscriptionHandleWrapper(
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
      MoQExecutor::SharedPtr exec)
      : moxygenHandle_(std::move(handle)), exec_(std::move(exec)) {}

  void unsubscribe() override {
    moxygenHandle_->unsubscribe();
  }

  void subscribeUpdate(
      SubscribeUpdate update,
      std::shared_ptr<vanilla::SubscribeUpdateCallback> callback) override {

    auto handle = moxygenHandle_;

    folly::coro::co_invoke(
        [handle, update = std::move(update), callback]()
            -> folly::coro::Task<void> {
          auto result = co_await handle->subscribeUpdate(update);
          if (result.hasValue()) {
            callback->onSubscribeUpdateOk(result.value());
          } else {
            callback->onSubscribeUpdateError(result.error());
          }
        })
        .scheduleOn(exec_.get())
        .start();
  }

  const SubscribeOk& subscribeOk() const override {
    return moxygenHandle_->subscribeOk();
  }
};
```

### Consumer Adapters

Bidirectional adapters for TrackConsumer, SubgroupConsumer, FetchConsumer:

- `MoxygenTrackConsumerWrapper` - wraps `moxygen::TrackConsumer`, implements `vanilla::TrackConsumer`
- `VanillaTrackConsumerWrapper` - wraps `vanilla::TrackConsumer`, implements `moxygen::TrackConsumer`
- Same pattern for SubgroupConsumer and FetchConsumer

**VanillaTrackConsumerWrapper - wraps vanilla consumer for use with moxygen:**

```cpp
// In adapters/ConsumerAdapters.cpp

class VanillaTrackConsumerWrapper : public moxygen::TrackConsumer {
  std::shared_ptr<vanilla::TrackConsumer> vanillaConsumer_;

public:
  explicit VanillaTrackConsumerWrapper(std::shared_ptr<vanilla::TrackConsumer> v)
      : vanillaConsumer_(std::move(v)) {}

  folly::Expected<std::shared_ptr<moxygen::SubgroupConsumer>, MoQPublishError>
  beginSubgroup(uint64_t groupID, uint64_t subgroupID, Priority pri) override {
    auto result = vanillaConsumer_->beginSubgroup(groupID, subgroupID, pri);
    if (result.hasValue()) {
      return std::make_shared<VanillaSubgroupConsumerWrapper>(result.value());
    }
    return folly::makeUnexpected(result.error());
  }

  folly::Expected<folly::Unit, MoQPublishError>
  objectStream(const ObjectHeader& header, Payload payload) override {
    // Convert moxygen Payload (IOBuf) to vanilla Payload - move, not clone
    auto vanillaPayload = iobufToPayload(std::move(payload));
    auto result = vanillaConsumer_->objectStream(header, std::move(vanillaPayload));
    if (result.hasValue()) {
      return folly::unit;
    }
    return folly::makeUnexpected(result.error());
  }

  folly::Expected<folly::Unit, MoQPublishError>
  datagram(const ObjectHeader& header, Payload payload) override {
    auto vanillaPayload = iobufToPayload(std::move(payload));
    auto result = vanillaConsumer_->datagram(header, std::move(vanillaPayload));
    if (result.hasValue()) {
      return folly::unit;
    }
    return folly::makeUnexpected(result.error());
  }

  folly::Expected<folly::Unit, MoQPublishError>
  setTrackAlias(TrackAlias alias) override {
    auto result = vanillaConsumer_->setTrackAlias(alias);
    return result.hasValue() ? folly::unit : folly::makeUnexpected(result.error());
  }

  folly::Expected<folly::Unit, MoQPublishError>
  subscribeDone(SubscribeDone done) override {
    auto result = vanillaConsumer_->subscribeDone(done);
    return result.hasValue() ? folly::unit : folly::makeUnexpected(result.error());
  }

  // Backpressure: bridge vanilla callback → moxygen coroutine
  folly::coro::Task<folly::Expected<uint64_t, MoQPublishError>>
  awaitReadyToConsume() override {
    auto [promise, future] = folly::coro::makePromiseContract<
        folly::Expected<uint64_t, MoQPublishError>>();

    class BridgeCallback : public vanilla::ReadyCallback {
      folly::coro::Promise<folly::Expected<uint64_t, MoQPublishError>> promise_;
    public:
      explicit BridgeCallback(
          folly::coro::Promise<folly::Expected<uint64_t, MoQPublishError>> p)
          : promise_(std::move(p)) {}

      void onReady(uint64_t bytes) override {
        promise_.setValue(bytes);
      }
      void onError(MoQPublishError err) override {
        promise_.setValue(folly::makeUnexpected(err));
      }
    };

    vanillaConsumer_->awaitReadyToConsume(
        std::make_shared<BridgeCallback>(std::move(promise)));
    co_return co_await std::move(future);
  }
};
```

**VanillaSubgroupConsumerWrapper:**

```cpp
class VanillaSubgroupConsumerWrapper : public moxygen::SubgroupConsumer {
  std::shared_ptr<vanilla::SubgroupConsumer> vanillaConsumer_;

public:
  explicit VanillaSubgroupConsumerWrapper(
      std::shared_ptr<vanilla::SubgroupConsumer> v)
      : vanillaConsumer_(std::move(v)) {}

  folly::Expected<folly::Unit, MoQPublishError>
  object(uint64_t objectID,
         Payload payload,
         Extensions extensions,
         bool finSubgroup) override {
    // Move payload, not clone
    auto vanillaPayload = iobufToPayload(std::move(payload));
    auto result = vanillaConsumer_->object(
        objectID, std::move(vanillaPayload), extensions, finSubgroup);
    return result.hasValue() ? folly::unit : folly::makeUnexpected(result.error());
  }

  folly::Expected<folly::Unit, MoQPublishError>
  objectNotExists(uint64_t objectID, bool finSubgroup) override {
    auto result = vanillaConsumer_->objectNotExists(objectID, finSubgroup);
    return result.hasValue() ? folly::unit : folly::makeUnexpected(result.error());
  }

  void checkpoint() override {
    vanillaConsumer_->checkpoint();
  }

  folly::Expected<folly::Unit, MoQPublishError>
  endOfSubgroup() override {
    auto result = vanillaConsumer_->endOfSubgroup();
    return result.hasValue() ? folly::unit : folly::makeUnexpected(result.error());
  }

  void reset(ResetStreamErrorCode error) override {
    vanillaConsumer_->reset(error);
  }
};
```

### Sample Usage

```cpp
#include <ti/experimental/moxygen/vanilla/Client.h>
#include <ti/experimental/moxygen/events/MoQFollyExecutorImpl.h>
#include <folly/io/async/EventBase.h>

using namespace moxygen;
using namespace moxygen::vanilla;

class MyConnectCallback : public ConnectCallback {
  Client* client_;
public:
  explicit MyConnectCallback(Client* c) : client_(c) {}

  void onConnected() override {
    std::cout << "Connected!\n";

    SubscribeRequest req;
    req.fullTrackName = FullTrackName{
        TrackNamespace{{"example"}}, "video"};
    req.priority = 128;
    req.groupOrder = GroupOrder::OLDEST_FIRST;

    auto consumer = std::make_shared<MyTrackConsumer>();
    auto callback = std::make_shared<MySubscribeCallback>();

    // Call subscribe directly on client (inherits Publisher)
    client_->subscribe(req, consumer, callback);
  }

  void onConnectError(SessionCloseErrorCode err, std::string reason) override {
    std::cerr << "Connect failed: " << reason << "\n";
  }
};

int main() {
  folly::EventBase evb;

  // Create executor (owned locally)
  auto exec = std::make_unique<MoQFollyExecutorImpl>(&evb);

  // Get KeepAlive token
  MoQExecutor::SharedPtr execPtr = exec->getSharedPtr();

  // Create client with KeepAlive
  vanilla::Client client(execPtr, "moqt://relay.example.com:4433");

  auto connectCb = std::make_shared<MyConnectCallback>(&client);
  client.connect(nullptr, nullptr, connectCb);

  evb.loopForever();
  return 0;
}
```

### vanilla::Server

Server inherits from MoQServer and overrides `onNewSession` to install vanilla<->moxygen adapters automatically. Users never see MoQSession, MoQExecutor, or adapters.

**Design Notes:**
- This design inherits from MoQServer specifically. Other `MoQServerBase` implementations (e.g., PicoQuic-based) would need their own vanilla wrapper following the same pattern (e.g., `vanilla::PicoQuicServer`).
- TODO: Expose a way for server handlers to access the underlying session (needed for relay use cases where cross-session coordination is required).

**Server.h:**

```cpp
#pragma once

#include <ti/experimental/moxygen/MoQServer.h>
#include <ti/experimental/moxygen/vanilla/Publisher.h>
#include <ti/experimental/moxygen/vanilla/Subscriber.h>
#include <functional>
#include <memory>

namespace moxygen::vanilla {

class Server : public MoQServer {
 public:
  // Factory creates handlers for each new session
  using SessionFactory = std::function<
      std::pair<std::shared_ptr<Publisher>, std::shared_ptr<Subscriber>>()>;

  // Function to customize ServerSetup response
  using SetupFunction = std::function<
      folly::Expected<ServerSetup, SessionCloseErrorCode>(
          const ClientSetup& clientSetup)>;

  // Inherit MoQServer constructors
  Server(
      std::string cert,
      std::string key,
      std::string endpoint,
      folly::Optional<quic::TransportSettings> transportSettings = folly::none)
      : MoQServer(
            std::move(cert),
            std::move(key),
            std::move(endpoint),
            transportSettings) {}

  Server(
      std::shared_ptr<const fizz::server::FizzServerContext> fizzContext,
      std::string endpoint,
      folly::Optional<quic::TransportSettings> transportSettings = folly::none)
      : MoQServer(std::move(fizzContext), std::move(endpoint), transportSettings) {}

  // Set the factory that creates Publisher/Subscriber for each session
  void setSessionFactory(SessionFactory factory) {
    sessionFactory_ = std::move(factory);
  }

  // Optional: customize ServerSetup negotiation
  void setSetupFunction(SetupFunction fn) {
    setupFn_ = std::move(fn);
  }

 protected:
  void onNewSession(std::shared_ptr<MoQSession> session) override;

  folly::Try<ServerSetup> onClientSetup(
      ClientSetup clientSetup,
      const std::shared_ptr<MoQSession>& session) override;

 private:
  SessionFactory sessionFactory_;
  SetupFunction setupFn_;
};

} // namespace moxygen::vanilla
```

**Server.cpp:**

```cpp
#include <ti/experimental/moxygen/vanilla/Server.h>
#include <ti/experimental/moxygen/vanilla/adapters/PublisherAdapters.h>
#include <ti/experimental/moxygen/vanilla/adapters/SubscriberAdapters.h>

namespace moxygen::vanilla {

folly::Try<ServerSetup> Server::onClientSetup(
    ClientSetup clientSetup,
    const std::shared_ptr<MoQSession>& session) {

  if (setupFn_) {
    auto result = setupFn_(clientSetup);
    if (result.hasError()) {
      return folly::Try<ServerSetup>(
          folly::make_exception_wrapper<std::runtime_error>("Setup rejected"));
    }
    return folly::Try<ServerSetup>(std::move(result.value()));
  }

  // Default: use base class behavior
  return MoQServer::onClientSetup(std::move(clientSetup), session);
}

void Server::onNewSession(std::shared_ptr<MoQSession> session) {
  if (!sessionFactory_) {
    XLOG(ERR) << "No session factory set";
    session->close();
    return;
  }

  auto [vanillaPub, vanillaSub] = sessionFactory_();
  auto exec = session->getExecutor();

  session->setPublishHandler(
      std::make_shared<VanillaPublisherWrapper>(vanillaPub, exec->getSharedPtr()));
  session->setSubscribeHandler(
      std::make_shared<VanillaSubscriberWrapper>(vanillaSub, exec->getSharedPtr()));

  MoQServer::onNewSession(session);
}

} // namespace moxygen::vanilla
```

**Sample Server Usage:**

```cpp
#include <ti/experimental/moxygen/vanilla/Server.h>

int main() {
  vanilla::Server server("cert.pem", "key.pem", "/moq");

  server.setSessionFactory([]() {
    return std::make_pair(
        std::make_shared<MyPublisher>(),
        std::make_shared<MySubscriber>());
  });

  // Optional: customize setup negotiation
  server.setSetupFunction([](const ClientSetup& clientSetup)
      -> folly::Expected<ServerSetup, SessionCloseErrorCode> {
    if (!isAuthorized(clientSetup)) {
      return folly::makeUnexpected(SessionCloseErrorCode::UNAUTHORIZED);
    }
    ServerSetup setup;
    setup.selectedVersion = pickVersion(clientSetup.supportedVersions);
    return setup;
  });

  server.start(folly::SocketAddress("0.0.0.0", 4433));
  return 0;
}
```

### Phase 7 Implementation Order

1. Create `adapters/` subdirectory with BUCK
2. Create adapter headers and implementations:
   - `PublisherAdapters.h/.cpp` - MoxygenPublisherWrapper, VanillaPublisherWrapper
   - `SubscriberAdapters.h/.cpp` - MoxygenSubscriberWrapper, VanillaSubscriberWrapper
   - `ConsumerAdapters.h/.cpp` - Track/Subgroup/Fetch consumer wrappers (both directions)
   - `HandleAdapters.h/.cpp` - SubscriptionHandle, FetchHandle, AnnounceHandle wrappers
3. Create `Client.h` with interface
4. Create `Client.cpp` with implementation using adapters
5. Create `Server.h` (inherits MoQServer)
6. Create `Server.cpp` with onNewSession/onClientSetup overrides
7. Update BUCK files
8. Build and test

---

## Complete File Structure

After all phases:

```
ti/experimental/moxygen/
├── Types.h                      # Core types extracted from MoQFramer.h (Phase 2)
├── MoQPublishError.h            # MoQPublishError extracted (Phase 4)
├── MoQFramer.h                  # Includes Types.h, framing logic only
├── events/
│   └── MoQExecutor.h            # Updated with SharedPtr (Phase 5)
└── vanilla/
    ├── BUCK
    ├── Expected.h               # Custom Expected<T, E> + Unit type
    ├── Payload.h                # Payload interface + VectorPayload + SharedPayload
    ├── Payload.cpp
    ├── IOBufPayload.h           # IOBufPayload (links folly)
    ├── IOBufPayload.cpp         # + payload conversion utilities
    ├── Callbacks.h              # All callback interface definitions
    ├── MoQConsumers.h           # TrackConsumer, SubgroupConsumer, FetchConsumer
    ├── Publisher.h              # Publisher, SubscriptionHandle, etc.
    ├── Subscriber.h             # Subscriber, AnnounceHandle, etc.
    ├── Client.h                 # vanilla::Client, ConnectCallback
    ├── Client.cpp               # Client implementation (PIMPL)
    ├── Server.h                 # vanilla::Server (inherits MoQServer)
    ├── Server.cpp               # Server implementation
    └── adapters/
        ├── BUCK
        ├── PublisherAdapters.h
        ├── PublisherAdapters.cpp
        ├── SubscriberAdapters.h
        ├── SubscriberAdapters.cpp
        ├── ConsumerAdapters.h
        ├── ConsumerAdapters.cpp
        ├── HandleAdapters.h
        └── HandleAdapters.cpp
```
