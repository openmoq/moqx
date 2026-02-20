# Moxygen Architecture, Compatibility Layer & Transport Pluggability

This document describes the moxygen architecture, the compatibility layer design, and how the various build modes work together.

## Table of Contents

- [Overview](#overview)
- [Build Modes](#build-modes)
- [Architecture Layers](#architecture-layers)
- [Transport Abstraction](#transport-abstraction)
- [Compatibility Layer](#compatibility-layer)
- [Transport Abstraction](#transport-abstraction)
- [Session Architecture](#session-architecture)
- [Build Configuration](#build-configuration)
- [Cross-Mode Interoperability](#cross-mode-interoperability)

---

## Overview

 The goal of this work is to support multiple build configurations to accommodate different deployment scenarios and enable easy
 integration of multiple quic stacks.
- **Full-featured mode**: Uses Folly library and mvfst QUIC stack for relay development
- **Minimal dependency mode**: Uses only C++ standard library and picoquic QUIC stack with minimal dependecies for relay development, easy integration into existing server systems and lightweight client-side deployments.
- **Hybrid mode**: Mix of Folly utilities with picoquic transport

```
┌─────────────────────────────────────────────────────────────────┐
│                        Application Layer                         │
│              (MoQDateServer, MoQTextClient, etc.)                │
├─────────────────────────────────────────────────────────────────┤
│                        MoQ Session Layer                         │
│     ┌─────────────────────┐    ┌─────────────────────────┐      │
│     │    MoQSession       │    │   MoQSessionCompat      │      │
│     │   (Coroutines)      │    │    (Callbacks)          │      │
│     └─────────────────────┘    └─────────────────────────┘      │
├─────────────────────────────────────────────────────────────────┤
│                     Transport Abstraction                        │
│                    WebTransportInterface                         │
├──────────────────────┬──────────────────────────────────────────┤
│   Proxygen/mvfst     │              Picoquic                     │
│   (WebTransport)     │    ┌────────────────┬─────────────────┐  │
│                      │    │ Raw Transport  │  H3 Transport   │  │
│                      │    │  (moq-00)      │  (WebTransport) │  │
└──────────────────────┴────┴────────────────┴─────────────────┴──┘
```

---

## Build Modes

### Mode 1: Folly + mvfst (Full Featured)

```
MOXYGEN_USE_FOLLY=ON
MOXYGEN_QUIC_BACKEND=mvfst
```

- **API style**: Folly coroutines (`folly::coro::Task`)
- **Dependencies**: Folly, Fizz, Wangle, mvfst, Proxygen
- **Build**: `getdeps.py build`
- **Binaries**: `moqrelayserver`, `moqdateserver`, `moqtextclient`

### Mode 2: std-mode + picoquic (Minimal Dependencies)

```
MOXYGEN_USE_FOLLY=OFF
MOXYGEN_QUIC_BACKEND=picoquic
```

- **API style**: Callbacks (`ResultCallback`, `VoidCallback`)
- **Dependencies**: OpenSSL (or mbedTLS), picoquic (fetched automatically)
- **Build**: `cmake -DMOXYGEN_USE_FOLLY=OFF`
- **Binaries**: `picodateserver`, `picotextclient`, `picorelayserver`

### Mode 3: Folly + picoquic (Hybrid)

```
MOXYGEN_USE_FOLLY=ON
MOXYGEN_QUIC_BACKEND=picoquic
```

- **API style**: Callbacks (picoquic doesn't integrate with Folly event loop)
- **Dependencies**: Folly + picoquic
- **Build**: `getdeps.py build` with `MOXYGEN_QUIC_BACKEND=picoquic`

### Mode Comparison

| Feature | Mode 1 | Mode 2 | Mode 3 |
|---------|--------|--------|--------|
| Coroutine support | ✅ | ❌ | ❌ |
| Callback support | ✅ | ✅ | ✅ |
| Minimal dependencies | ❌ | ✅ | ❌ |
| Fast build time | ❌ (~30 min) | ✅ (~2 min) | ❌ |
| Production ready | ✅ | ✅ | ✅ |
| WebTransport | ✅ | ✅ | ✅ |
| Raw QUIC | ✅ | ✅ | ✅ |

---

## Architecture Layers

### Layer 1: Application

Sample applications demonstrating MoQ usage:

```
moxygen/samples/
├── date/           # Date publisher server
├── text-client/    # Text subscriber client
├── relay/          # MoQ relay/broker server
├── flv_streamer/   # FLV media publisher
└── flv_receiver/   # FLV media receiver
```

### Layer 2: MoQ Session

The session layer handles MoQ protocol semantics:

```
moxygen/
├── MoQSession.h          # Coroutine-based session (Folly mode)
├── MoQSessionCompat.cpp  # Callback-based session (all modes)
├── MoQSessionBase.h      # Shared base functionality
├── MoQCodec.cpp          # Wire format encoding/decoding
├── MoQFramer.cpp         # Message framing
└── MoQTypes.cpp          # Protocol types and structures
```

### Layer 3: Transport

Transport implementations providing QUIC/WebTransport:

```
moxygen/transports/
└── openmoq/
    ├── picoquic/
    │   ├── PicoquicRawTransport.cpp   # Raw QUIC (ALPN: moq-00)
    │   ├── PicoquicH3Transport.cpp    # HTTP/3 WebTransport
    │   ├── PicoquicMoQClient.cpp      # Client abstraction
    │   └── PicoquicMoQServer.cpp      # Server abstraction
    └── adapters/
        ├── ProxygenWebTransportAdapter.cpp  # Proxygen integration
        └── MvfstStdModeAdapter.cpp          # mvfst with callbacks
```

### Layer 4: Compatibility

Abstractions enabling cross-mode compatibility:

```
moxygen/compat/
├── ByteBuffer.h          # Buffer management (vs folly::IOBuf)
├── ByteBufferQueue.h     # Buffer chains
├── ByteCursor.h          # Zero-copy parsing
├── Expected.h            # Error handling (vs folly::Expected)
├── Try.h                 # Exception wrapper
├── Async.h               # Futures/promises
├── Callbacks.h           # Callback interfaces
├── Containers.h          # Hash maps/sets
├── Debug.h               # Logging (vs XLOG)
└── MoQPriorityQueue.h    # Priority scheduling
```

---

## Transport Abstraction

### Design Goals

The `WebTransportInterface` abstraction enables moxygen to support multiple QUIC stacks without changing the MoQ session layer. Key goals:

1. **QUIC Stack Independence**: MoQSession code works with any QUIC implementation
2. **Transport Mode Flexibility**: Support both Raw QUIC and HTTP/3 WebTransport
3. **API Consistency**: Same callback-based API regardless of underlying transport
4. **Zero-Copy Where Possible**: Efficient buffer handling across all implementations

### Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           MoQSession / MoQSessionCompat                      │
│                      (Protocol logic, subscribe/publish)                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                          WebTransportInterface                               │
│   createUniStream() | createBidiStream() | sendDatagram() | closeSession()  │
├──────────────────┬──────────────────┬──────────────────┬────────────────────┤
│  PicoquicRaw     │  PicoquicH3      │  ProxygenWT      │  MvfstStdMode      │
│  Transport       │  Transport       │  Adapter         │  Adapter           │
│  (moq-00)        │  (h3/WT)         │  (h3/WT)         │  (h3/WT)           │
├──────────────────┴──────────────────┼──────────────────┴────────────────────┤
│          picoquic QUIC stack        │           mvfst QUIC stack            │
│          (C, minimal deps)          │           (C++, Folly-based)          │
└─────────────────────────────────────┴───────────────────────────────────────┘
```

### WebTransportInterface API

The interface defines all operations needed for MoQ transport:

```cpp
namespace moxygen::compat {

class WebTransportInterface {
 public:
  virtual ~WebTransportInterface() = default;

  // === Stream Creation ===

  // Create outgoing unidirectional stream (for SUBSCRIBE, OBJECT headers)
  virtual Expected<StreamWriteHandle*, WebTransportError> createUniStream() = 0;

  // Create outgoing bidirectional stream (for control messages)
  virtual Expected<BidiStreamHandle*, WebTransportError> createBidiStream() = 0;

  // Wait for stream creation credit (MAX_STREAMS flow control)
  virtual Expected<SemiFuture<Unit>, WebTransportError> awaitUniStreamCredit();
  virtual Expected<SemiFuture<Unit>, WebTransportError> awaitBidiStreamCredit();

  // === Datagrams ===

  virtual Expected<Unit, WebTransportError> sendDatagram(
      std::unique_ptr<Payload> data) = 0;
  virtual void setMaxDatagramSize(size_t maxSize) = 0;
  virtual size_t getMaxDatagramSize() const = 0;

  // === Session Control ===

  virtual void closeSession(uint32_t errorCode = 0) = 0;
  virtual void drainSession() = 0;

  // === Connection Info ===

  virtual SocketAddress getPeerAddress() const = 0;
  virtual SocketAddress getLocalAddress() const = 0;
  virtual std::string getALPN() const = 0;  // "moq-00" or "h3"
  virtual bool isConnected() const = 0;

  // === Event Callbacks ===

  // Incoming streams from peer
  virtual void setNewUniStreamCallback(
      std::function<void(StreamReadHandle*)> cb) = 0;
  virtual void setNewBidiStreamCallback(
      std::function<void(BidiStreamHandle*)> cb) = 0;

  // Incoming datagrams
  virtual void setDatagramCallback(
      std::function<void(std::unique_ptr<Payload>)> cb) = 0;

  // Session lifecycle
  virtual void setSessionCloseCallback(
      std::function<void(std::optional<uint32_t> error)> cb) = 0;
  virtual void setSessionDrainCallback(std::function<void()> cb) = 0;
};

}  // namespace moxygen::compat
```

### Stream Handle Interfaces

Separate handles for reading and writing enable fine-grained flow control:

```cpp
// Write handle - for sending data on a stream
class StreamWriteHandle {
 public:
  virtual uint64_t getID() const = 0;

  // Async write with completion callback
  virtual void writeStreamData(
      std::unique_ptr<Payload> data,
      bool fin,
      std::function<void(bool success)> callback = nullptr) = 0;

  // Sync write (for picoquic JIT send model)
  virtual Expected<Unit, WebTransportError> writeStreamDataSync(
      std::unique_ptr<Payload> data, bool fin) = 0;

  virtual void resetStream(uint32_t errorCode) = 0;
  virtual void setPriority(const StreamPriority& priority) = 0;

  // Flow control: wait until writable
  virtual void awaitWritable(std::function<void()> callback) = 0;
  virtual Expected<SemiFuture<Unit>, WebTransportError> awaitWritable();

  // Peer cancellation
  virtual void setPeerCancelCallback(std::function<void(uint32_t)> cb) = 0;
  virtual bool isCancelled() const = 0;

  // Delivery tracking (for reliability)
  virtual Expected<Unit, WebTransportError> registerDeliveryCallback(
      uint64_t offset, DeliveryCallback* cb) = 0;
};

// Read handle - for receiving data from a stream
class StreamReadHandle {
 public:
  virtual uint64_t getID() const = 0;

  // Set callback for incoming data
  virtual void setReadCallback(
      std::function<void(StreamData, std::optional<uint32_t> error)> cb) = 0;

  virtual void pauseReading() = 0;
  virtual void resumeReading() = 0;

  // Send STOP_SENDING to peer
  virtual Expected<Unit, WebTransportError> stopSending(uint32_t error) = 0;

  virtual bool isFinished() const = 0;
};

// Bidirectional stream combines both
class BidiStreamHandle {
 public:
  virtual StreamWriteHandle* writeHandle() = 0;
  virtual StreamReadHandle* readHandle() = 0;
};
```

### Implementation Details

#### 1. PicoquicRawTransport (Raw QUIC)

Direct QUIC streams without HTTP/3 framing:

```
┌─────────────────────────────────────────────────────────┐
│                  PicoquicRawTransport                   │
├─────────────────────────────────────────────────────────┤
│ ALPN: "moq-00"                                          │
│ Streams: Native QUIC stream IDs                         │
│ Send Model: JIT (mark_active_stream + prepare_to_send)  │
│ Threading: PicoquicThreadDispatcher for thread safety   │
└─────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────┐
│                   picoquic QUIC API                     │
│   picoquic_cnx_t* | picoquic_callback | stream APIs    │
└─────────────────────────────────────────────────────────┘
```

Key implementation aspects:

- **JIT Send Model**: Data is buffered in `OutBuffer` and provided to picoquic
  when `picoquic_callback_prepare_to_send` fires (flow control aware)
- **Thread Dispatcher**: All picoquic API calls happen on the picoquic network
  thread via `PicoquicThreadDispatcher::runOnPicoThread()`
- **Stream Tracking**: Maps `uint64_t streamId` to `PicoquicStreamWriteHandle`
  and `PicoquicStreamReadHandle`

```cpp
class PicoquicStreamWriteHandle : public StreamWriteHandle {
  // JIT send buffer
  struct OutBuffer {
    std::mutex mutex;
    std::deque<std::unique_ptr<Payload>> chunks;
    size_t firstChunkOffset{0};  // Partial send tracking
    bool finQueued{false};
    bool finSent{false};
    std::vector<Promise<Unit>> readyWaiters;  // Backpressure
  };

  // Called by picoquic when flow control allows sending
  int onPrepareToSend(void* context, size_t maxLength) {
    std::lock_guard lock(outbuf_.mutex);
    // Provide buffered data to picoquic
    // Wake any waiters blocked on backpressure
  }
};
```

#### 2. PicoquicH3Transport (HTTP/3 WebTransport)

HTTP/3 WebTransport using picoquic's h3zero library:

```
┌─────────────────────────────────────────────────────────┐
│                  PicoquicH3Transport                    │
├─────────────────────────────────────────────────────────┤
│ ALPN: "h3"                                              │
│ Session: HTTP/3 CONNECT to establish WT session         │
│ Streams: WebTransport stream framing over QUIC          │
│ Interop: Compatible with mvfst/proxygen servers         │
└─────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────┐
│              h3zero (picoquic HTTP/3)                   │
│   h3zero_callback_ctx_t | CONNECT handling | framing   │
└─────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────┐
│                   picoquic QUIC API                     │
└─────────────────────────────────────────────────────────┘
```

WebTransport session establishment (client-side):

```cpp
bool PicoquicH3Transport::initiateWebTransportUpgrade() {
  // 1. Create HTTP/3 context
  h3Ctx_ = h3zero_callback_create_context(...);

  // 2. Send CONNECT request to establish WebTransport session
  //    Path: "/moq" or "/moq-relay" depending on endpoint
  h3zero_send_connect_request(cnx_, path_, ...);

  // 3. Wait for CONNECT response (200 OK = session established)
  // 4. All subsequent streams are WebTransport streams
}
```

#### 3. ProxygenWebTransportAdapter (mvfst/Proxygen)

Wraps Proxygen's WebTransport API for Mode 1 builds:

```
┌─────────────────────────────────────────────────────────┐
│              ProxygenWebTransportAdapter                │
├─────────────────────────────────────────────────────────┤
│ Wraps: proxygen::WebTransport                           │
│ Event Loop: Folly EventBase                             │
│ Streams: proxygen::WebTransport::StreamHandle           │
└─────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────┐
│               proxygen::WebTransport                    │
│        (Full HTTP/3 WebTransport implementation)        │
└─────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────┐
│                  mvfst QUIC stack                       │
│              (Folly-based, coroutine-friendly)          │
└─────────────────────────────────────────────────────────┘
```

The adapter translates between APIs:

```cpp
class ProxygenWebTransportAdapter : public WebTransportInterface {
  folly::MaybeManagedPtr<proxygen::WebTransport> wt_;

  Expected<StreamWriteHandle*, WebTransportError> createUniStream() override {
    // Call proxygen API and wrap result
    auto streamId = wt_->createUnidirectionalStream();
    if (!streamId) {
      return makeUnexpected(WebTransportError::STREAM_CREATION_ERROR);
    }
    auto handle = std::make_unique<ProxygenStreamWriteHandle>(
        wt_->getStreamWriteHandle(*streamId));
    // Store and return...
  }
};
```

#### 4. MvfstStdModeAdapter (Hybrid Mode)

Enables std-mode code to use mvfst without exposing Folly types:

```
┌─────────────────────────────────────────────────────────┐
│                 MvfstStdModeAdapter                     │
├─────────────────────────────────────────────────────────┤
│ API: Callback-based (no Folly types exposed)            │
│ Internal: Owns Folly EventBase thread                   │
│ Threading: Marshals calls to EventBase thread           │
└─────────────────────────────────────────────────────────┘
         │
         ▼ (internal, hidden from API)
┌─────────────────────────────────────────────────────────┐
│     folly::ScopedEventBaseThread (owned internally)     │
├─────────────────────────────────────────────────────────┤
│               proxygen::WebTransport                    │
├─────────────────────────────────────────────────────────┤
│                  mvfst QUIC stack                       │
└─────────────────────────────────────────────────────────┘
```

This enables "Mode 4" style builds where you want mvfst performance
but std-mode API compatibility:

```cpp
// User code (std-mode, no Folly includes)
auto adapter = MvfstStdModeAdapter::create(config);
adapter->connect([](bool success) {
  // Use adapter as WebTransportInterface
  adapter->createUniStream();  // Marshaled to EventBase thread
});
```

### Transport Selection Matrix

| Implementation | QUIC Stack | ALPN | Use Case |
|---------------|------------|------|----------|
| `PicoquicRawTransport` | picoquic | moq-00 | Direct MoQ, no HTTP/3 overhead |
| `PicoquicH3Transport` | picoquic | h3 | Interop with other WebTransport impls |
| `ProxygenWebTransportAdapter` | mvfst | h3 | Full-featured Folly mode |
| `MvfstStdModeAdapter` | mvfst | h3 | std-mode API with mvfst performance |

### Thread Safety Model

Each implementation handles threading differently:

| Implementation | Threading Model |
|---------------|-----------------|
| `PicoquicRawTransport` | `PicoquicThreadDispatcher` marshals to pico thread |
| `PicoquicH3Transport` | Same as above |
| `ProxygenWebTransportAdapter` | Folly EventBase (caller must be on evb) |
| `MvfstStdModeAdapter` | Internal EventBase, callbacks on user thread |

### Flow Control Integration

The interface exposes flow control through several mechanisms:

1. **Stream Creation Credit**: `awaitUniStreamCredit()` / `awaitBidiStreamCredit()`
   returns a future that completes when MAX_STREAMS allows creation

2. **Write Backpressure**: `awaitWritable()` on `StreamWriteHandle` signals
   when flow control allows more data

3. **JIT Send Model** (picoquic): Data buffered until `prepare_to_send` callback
   indicates flow credits are available

```cpp
// Example: Writing with backpressure handling
void sendObject(StreamWriteHandle* stream, Payload data) {
  auto result = stream->writeStreamDataSync(std::move(data), false);
  if (!result && result.error() == WebTransportError::BLOCKED) {
    // Wait for flow control
    stream->awaitWritable([this, stream]() {
      // Retry send
    });
  }
}
```

### Cross-Mode Interoperability

WebTransportInterface enables interop between different build modes:

```
┌─────────────────────┐                    ┌─────────────────────┐
│   picoquic Client   │                    │   mvfst Server      │
│  (PicoquicH3Transport)                   │  (proxygen WT)      │
│                     │◄──── h3/WT ────────│                     │
│   Mode 2 binary     │                    │   Mode 1 binary     │
└─────────────────────┘                    └─────────────────────┘

Both use WebTransportInterface internally, but communicate via
standard HTTP/3 WebTransport protocol on the wire.
```

Path configuration is critical for interop:
- mvfst relay: `/moq-relay`
- mvfst date server: path set via `--ns` flag
- picoquic default: `/moq`
- Use `--path` flag on picoquic client to match server

---

## Compatibility Layer

The compat layer provides Folly-equivalent functionality using only C++ standard library:

### ByteBuffer (vs folly::IOBuf)

```cpp
// Folly mode
std::unique_ptr<folly::IOBuf> buf = folly::IOBuf::create(1024);

// Std mode (compat)
moxygen::compat::ByteBuffer buf(1024);
```

Key features:
- **Headroom/tailroom**: O(1) prepend and trimStart operations
- **Small buffer optimization**: Inline storage for buffers ≤64 bytes
- **Buffer chaining**: Link multiple buffers without copying
- **Reference counting**: Shared ownership for zero-copy

### Expected (vs folly::Expected)

```cpp
// Folly mode
folly::Expected<int, Error> result = compute();

// Std mode (compat)
moxygen::compat::Expected<int, Error> result = compute();
```

### Async Primitives

```cpp
// Folly mode
folly::coro::Task<Result> doWork();

// Std mode (compat) - callback based
void doWork(ResultCallback<Result, Error> callback);
```

### Conditional Compilation

```cpp
#if MOXYGEN_USE_FOLLY
  #include <folly/io/IOBuf.h>
  using Payload = std::unique_ptr<folly::IOBuf>;
#else
  #include <moxygen/compat/ByteBuffer.h>
  using Payload = moxygen::compat::ByteBuffer;
#endif
```

---



## Session Architecture

### Coroutine-based Session (Mode 1)

```cpp
class MoQSession {
 public:
  // Subscribe returns a coroutine
  folly::coro::Task<SubscriptionHandle> subscribe(SubscribeRequest req);

  // Publish returns a coroutine
  folly::coro::Task<void> publish(PublishRequest req);
};

// Usage
folly::coro::Task<void> example() {
  auto handle = co_await session->subscribe(req);
  while (auto obj = co_await handle->objects()) {
    process(obj);
  }
}
```

### Callback-based Session (All Modes)

```cpp
class MoQSessionCompat {
 public:
  // Subscribe with callback
  void subscribe(
    SubscribeRequest req,
    std::shared_ptr<SubscribeCallback> callback);

  // Publish with callback
  void publish(
    PublishRequest req,
    std::shared_ptr<PublishCallback> callback);
};

// Usage
session->subscribe(req, makeCallback(
  [](SubscriptionHandle handle) {
    handle->setObjectCallback([](Object obj) {
      process(obj);
    });
  },
  [](Error err) {
    handleError(err);
  }
));
```

### Session State Machine

```
┌─────────┐     SETUP      ┌───────────┐
│  INIT   │───────────────>│  SETUP    │
└─────────┘                └───────────┘
                                 │
                           SERVER_SETUP
                                 │
                                 v
┌─────────┐    GOAWAY      ┌───────────┐
│ CLOSED  │<───────────────│  ACTIVE   │
└─────────┘                └───────────┘
                                 │
                          SUBSCRIBE/PUBLISH
                                 │
                                 v
                           ┌───────────┐
                           │ STREAMING │
                           └───────────┘
```

---

## Build Configuration

### CMake Options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `MOXYGEN_USE_FOLLY` | ON/OFF | ON | Use Folly library |
| `MOXYGEN_QUIC_BACKEND` | mvfst/picoquic | mvfst | QUIC implementation |
| `MOXYGEN_TLS_BACKEND` | openssl/mbedtls | openssl | TLS library (picoquic only) |
| `BUILD_TESTS` | ON/OFF | ON | Build test suite |

### Build Commands

```bash
# Mode 1: Folly + mvfst
./build/fbcode_builder/getdeps.py build --src-dir=moxygen:. moxygen \
  --allow-system-packages --scratch-path _build

# Mode 2: std-mode + picoquic
cmake -S . -B _build_std \
  -DMOXYGEN_USE_FOLLY=OFF \
  -DMOXYGEN_QUIC_BACKEND=picoquic
cmake --build _build_std --parallel

# Mode 3: Folly + picoquic
./build/fbcode_builder/getdeps.py build --src-dir=moxygen:. moxygen \
  --allow-system-packages --scratch-path _build_folly_pico \
  --extra-cmake-defines '{"MOXYGEN_QUIC_BACKEND":"picoquic"}'
```
### Build Sizes & Dependencies

#### Binary Sizes

| Binary | Mode 1 (Folly+mvfst) | Mode 2 (std+picoquic) | Mode 3 (Folly+picoquic) |
|--------|---------------------|----------------------|------------------------|
| Date Server | 11 MB | 2.5 MB | 3.4 MB |
| Relay Server | 11 MB | 2.3 MB | 3.0 MB |
| Text Client | 8.7 MB | 2.3 MB | 3.1 MB |

#### Build Directory Sizes

| Mode | Total Size | Dependencies |
|------|-----------|--------------|
| Mode 1 (Folly + mvfst) | 5.4 GB | 21 packages |
| Mode 2 (std + picoquic) | 179 MB | 3 packages |
| Mode 3 (Folly + picoquic) | 4.8 GB | 36 packages |

#### Dependency Trees

##### Mode 1: Folly + mvfst

```
moxygen
├── proxygen (HTTP/3, WebTransport)
│   ├── mvfst (QUIC stack)
│   │   ├── fizz (TLS 1.3)
│   │   │   └── folly
│   │   └── folly
│   ├── wangle (async framework)
│   │   └── folly
│   └── folly
├── folly
│   ├── glog (logging)
│   ├── gflags (command line)
│   ├── double-conversion
│   ├── libevent
│   ├── snappy (compression)
│   └── zlib
├── googletest (testing)
├── libdwarf (debug info)
├── liboqs (post-quantum crypto)
└── c-ares (async DNS)
```

##### Mode 2: std + picoquic

```
moxygen
├── picoquic (QUIC stack)
│   └── picotls (TLS 1.3)
│       └── OpenSSL
└── spdlog (logging, header-only)
```

##### Mode 3: Folly + picoquic

```
moxygen
├── picoquic (QUIC stack)
│   └── picotls (TLS 1.3)
├── folly
│   ├── glog
│   ├── gflags
│   ├── double-conversion
│   ├── libevent
│   ├── snappy
│   └── zlib
├── proxygen (HTTP utilities only)
├── wangle
├── fizz
├── googletest
├── libdwarf
├── liboqs
└── c-ares
```

#### Build Time Comparison

| Mode | Approximate Build Time | Notes |
|------|----------------------|-------|
| Mode 1 | ~30 minutes | Full dependency build with getdeps.py |
| Mode 2 | ~2 minutes | Minimal deps, picoquic fetched via CMake |
| Mode 3 | ~30 minutes | Full Folly stack + picoquic |

---

## Cross-Mode Interoperability

### WebTransport Interop

Different build modes can communicate via WebTransport (ALPN: `h3`):

```
┌─────────────────┐         WebTransport          ┌─────────────────┐
│  Mode 2 Client  │◄─────────── h3 ──────────────►│  Mode 1 Server  │
│  (picoquic)     │                               │  (mvfst)        │
└─────────────────┘                               └─────────────────┘
```

### Path Configuration

WebTransport requires matching endpoint paths:

| Component | Default Path | Flag |
|-----------|--------------|------|
| mvfst relay | `/moq-relay` | Built-in |
| mvfst date server | `/moq-date` | `--ns` sets path |
| picoquic server | `/moq` | Default |
| picoquic client | `/moq` | `--path` |

### Interop Matrix

| Client | Server | Raw QUIC | WebTransport |
|--------|--------|----------|--------------|
| mvfst | mvfst | ✅ | ✅ |
| picoquic | picoquic | ✅ | ✅ |
| picoquic | mvfst | ✅ | ✅ |
| mvfst | picoquic | ✅ | ✅ |


---

## Testing

### Automated Test Script

```bash
# Run all tests
./scripts/test_all_modes.sh

# Skip builds, just test
./scripts/test_all_modes.sh --skip-build

# Test specific mode
./scripts/test_all_modes.sh --mode1-only
./scripts/test_all_modes.sh --mode2-only
```

### Manual Testing

#### Mode 1: Folly + mvfst

**Direct Connection - Raw QUIC:**
```bash
# Terminal 1: Server
moqdateserver --port 4433 --cert cert.pem --key key.pem --ns moq-date

# Terminal 2: Client
moqtextclient --connect_url "https://localhost:4433/moq-date" \
  --track_namespace moq-date --track_name date --insecure
```

**Direct Connection - WebTransport:**
```bash
# Terminal 1: Server (WebTransport is default for mvfst)
moqdateserver --port 4433 --cert cert.pem --key key.pem --ns moq-date

# Terminal 2: Client
moqtextclient --connect_url "https://localhost:4433/moq-date" \
  --track_namespace moq-date --track_name date --insecure
```

**Via Relay - Raw QUIC:**
```bash
# Terminal 1: Relay
moqrelayserver --port 4433 --cert cert.pem --key key.pem --insecure

# Terminal 2: Publisher
moqdateserver --port 4434 --cert cert.pem --key key.pem \
  --relay_url "https://localhost:4433/moq-relay" --ns moq-date --insecure

# Terminal 3: Subscriber
moqtextclient --connect_url "https://localhost:4433/moq-relay" \
  --track_namespace moq-date --track_name date --insecure
```

#### Mode 2: std + picoquic

**Direct Connection - Raw QUIC:**
```bash
# Terminal 1: Server
picodateserver --port 4433 --cert cert.pem --key key.pem \
  --ns moq-date --mode spg --transport quic

# Terminal 2: Client
picotextclient --host localhost --port 4433 \
  --ns moq-date --track date --transport quic --insecure
```

**Direct Connection - WebTransport:**
```bash
# Terminal 1: Server
picodateserver --port 4433 --cert cert.pem --key key.pem \
  --ns moq-date --mode spg --transport webtransport

# Terminal 2: Client
picotextclient --host localhost --port 4433 \
  --ns moq-date --track date --transport webtransport --insecure
```

**Via Relay - Raw QUIC:**
```bash
# Terminal 1: Relay
picorelayserver --port 4433 --cert cert.pem --key key.pem --transport quic

# Terminal 2: Publisher
picodateserver --port 4434 --cert cert.pem --key key.pem \
  --relay_url "quic://localhost:4433" --ns moq-date --mode spg --transport quic

# Terminal 3: Subscriber
picotextclient --host localhost --port 4433 \
  --ns moq-date --track date --transport quic --insecure
```

**Via Relay - WebTransport:**
```bash
# Terminal 1: Relay
picorelayserver --port 4433 --cert cert.pem --key key.pem --transport webtransport

# Terminal 2: Publisher
picodateserver --port 4434 --cert cert.pem --key key.pem \
  --relay_url "https://localhost:4433/moq" --ns moq-date --mode spg --transport webtransport

# Terminal 3: Subscriber
picotextclient --host localhost --port 4433 --path /moq \
  --ns moq-date --track date --transport webtransport --insecure
```

#### Cross-Mode Interop

**picoquic client → mvfst server (WebTransport):**
```bash
# Terminal 1: mvfst relay
moqrelayserver --port 4433 --cert cert.pem --key key.pem --insecure

# Terminal 2: mvfst publisher
moqdateserver --port 4434 --cert cert.pem --key key.pem \
  --relay_url "https://localhost:4433/moq-relay" --ns moq-date --insecure

# Terminal 3: picoquic subscriber
picotextclient --host localhost --port 4433 --path /moq-relay \
  --ns moq-date --track date --transport webtransport --insecure
```

**mvfst client → picoquic server (WebTransport):**
```bash
# Terminal 1: picoquic server
picodateserver --port 4433 --cert cert.pem --key key.pem \
  --ns moq-date --mode spg --transport webtransport

# Terminal 2: mvfst client
moqtextclient --connect_url "https://localhost:4433/moq" \
  --track_namespace moq-date --track_name date --insecure
```

---

## Next Steps

- [ ] Mode 4: std-mode + mvfst (requires mvfst callback adapter)
- [ ] QUIC datagram support for picoquic ( easy)
- [ ] Connection migration
- [ ] Congestion control tuning per mode
