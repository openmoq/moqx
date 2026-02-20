# Moxygen Architecture & Compatibility Layer

This document describes the moxygen architecture, the compatibility layer design, and how the various build modes work together.

## Table of Contents

- [Overview](#overview)
- [Build Modes](#build-modes)
- [Architecture Layers](#architecture-layers)
- [Compatibility Layer](#compatibility-layer)
- [Transport Abstraction](#transport-abstraction)
- [Session Architecture](#session-architecture)
- [Build Configuration](#build-configuration)
- [Cross-Mode Interoperability](#cross-mode-interoperability)

---

## Overview

The goal of this work is to support multiple build configurations to accommodate different deployment scenarios:

- **Full-featured mode**: Uses Folly library and mvfst QUIC stack for relay development
- **Minimal dependency mode**: Uses only C++ standard library and picoquic QUIC stack with minimal dependecies for
    relay development, easy integration into existing server systems and lightweight client-side deployments.
- **Hybrid mode**: Mix of Folly utilities with picoquic transport

```
┌─────────────────────────────────────────────────────────────────┐
│                        Application Layer                        │
│              (MoQDateServer, MoQTextClient, etc.)               │
├─────────────────────────────────────────────────────────────────┤
│                        MoQ Session Layer                        │
│     ┌─────────────────────┐    ┌─────────────────────────┐      │
│     │    MoQSession       │    │   MoQSessionCompat      │      │
│     │   (Coroutines)      │    │    (Callbacks)          │      │
│     └─────────────────────┘    └─────────────────────────┘      │
├─────────────────────────────────────────────────────────────────┤
│                     Transport Abstraction                       │
│                    WebTransportInterface                        │
├──────────────────────┬──────────────────────────────────────────┤
│   Proxygen/mvfst     │              Picoquic                    │
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

## Transport Abstraction

### WebTransportInterface

Common interface for all transports:

```cpp
class WebTransportInterface {
 public:
  // Stream management
  virtual StreamId createBidiStream() = 0;
  virtual StreamId createUniStream() = 0;

  // Data transfer
  virtual void writeStreamData(StreamId id, ByteBuffer data, bool fin) = 0;
  virtual void readStreamData(StreamId id, ReadCallback* cb) = 0;

  // Connection state
  virtual bool isConnected() const = 0;
  virtual void close(uint32_t errorCode) = 0;
};
```

### Transport Implementations

#### Raw QUIC Transport (PicoquicRawTransport)

- ALPN: `moq-00`
- Direct QUIC streams for MoQ messages
- Lower overhead, no HTTP/3 framing
- Not interoperable across implementations

#### HTTP/3 WebTransport (PicoquicH3Transport)

- ALPN: `h3`
- HTTP/3 CONNECT for session establishment
- WebTransport datagrams and streams
- Interoperable with other WebTransport implementations

#### Proxygen WebTransport (Mode 1 only)

- Uses Proxygen's HTTP/3 implementation
- Integrated with Folly event loop
- Full WebTransport compliance

### Transport Selection

```cpp
// Mode 2/3: Picoquic
if (transportMode == TransportMode::RawQuic) {
  transport = std::make_unique<PicoquicRawTransport>(cnx);
} else {
  transport = std::make_unique<PicoquicH3Transport>(cnx, path);
}

// Mode 1: Proxygen
transport = std::make_unique<ProxygenWebTransport>(session);
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

## Build Sizes & Dependencies

### Binary Sizes

| Binary | Mode 1 (Folly+mvfst) | Mode 2 (std+picoquic) | Mode 3 (Folly+picoquic) |
|--------|---------------------|----------------------|------------------------|
| Date Server | 11 MB | 2.5 MB | 3.4 MB |
| Relay Server | 11 MB | 2.3 MB | 3.0 MB |
| Text Client | 8.7 MB | 2.3 MB | 3.1 MB |

### Build Directory Sizes

| Mode | Total Size | Dependencies |
|------|-----------|--------------|
| Mode 1 (Folly + mvfst) | 5.4 GB | 21 packages |
| Mode 2 (std + picoquic) | 179 MB | 3 packages |
| Mode 3 (Folly + picoquic) | 4.8 GB | 36 packages |

### Dependency Trees

#### Mode 1: Folly + mvfst

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

#### Mode 2: std + picoquic

```
moxygen
├── picoquic (QUIC stack)
│   └── picotls (TLS 1.3)
│       └── OpenSSL
└── spdlog (logging, header-only)
```

#### Mode 3: Folly + picoquic

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

### Build Time Comparison

| Mode | Approximate Build Time | Notes |
|------|----------------------|-------|
| Mode 1 | ~30 minutes | Full dependency build with getdeps.py |
| Mode 2 | ~2 minutes | Minimal deps, picoquic fetched via CMake |
| Mode 3 | ~30 minutes | Full Folly stack + picoquic |

---

## Next Steps

- [ ] Mode 4: std-mode + mvfst (requires mvfst callback adapter)
- [ ] QUIC datagram support for picoquic ( easy)
- [ ] Connection migration
- [ ] Congestion control tuning per mode
