# Plan: Stats Collection and Admin Endpoint for o-rly

## Context

o-rly is a MoQ relay built on moxygen. The codebase is fully functional:
- `ORelay` (`include/o_rly/ORelay.h`, `src/ORelay.cpp`) — relay logic
- `ORelayServer` (`include/o_rly/ORelayServer.h`, `src/ORelayServer.cpp`) — subclasses `moxygen::MoQServer`; `onNewSession()` / `terminateClientSession()` are the session lifecycle hooks
- `main.cpp` — full `folly::Init`, flags, `evb.loopForever()`

moxygen hooks already exist — **no moxygen changes required**:
- `MoQSession::setPublisherStatsCallback()` / `setSubscriberStatsCallback()` (`MoQSession.h:260-267`)
- `MoQServer::setQuicStatsFactory()` (`MoQServer.h:57`)

Admin server uses `proxygen/httpserver/HTTPServer.h` (classic RequestHandler API) for now; future migration to `proxygen/lib/http/coro/server/HTTPServer.h`.

---

## Threading Model for Stats

The relay is currently **single-threaded** (one event loop). Key facts:
- **MoQ stats callbacks** fire on the relay's executor — single producer; plain counters, no synchronization on writes.
- **QUIC stats callbacks** fire on QUIC worker threads. mvfst creates **one `QuicTransportStatsCallback` instance per worker** via the factory — each is also single-producer.
- **HTTP reader** is on the admin server thread — aggregates across collectors on-demand.

**Write path (hot): plain counter increment — zero overhead, single `ADD` instruction.**

**Read path (cold, Prometheus scrapes every 15-60s): folly::coro on-demand aggregation.**

Each collector holds a `folly::Executor::KeepAlive<>` to its owning executor. `StatsRegistry::aggregateAsync()` is a `folly::coro::Task<StatsSnapshot>`:
1. Briefly locks registry mutex to copy the collector list; releases.
2. Builds one `folly::coro::Task<StatsSnapshot>` per collector, each `scheduleOn(collector->owningExecutor())`. The task calls `collector->snapshot()` — runs on the collector's own thread, no data race, no atomics.
3. `co_await folly::coro::collectAll(std::move(tasks))` — parallel fan-out/fan-in.
4. Sums snapshots via `StatsSnapshot::operator+=` and returns.

Registry mutex is **only** for the collector list (adds/removes at session connect/disconnect).

**Caching:** `AdminServer` caches the last `StatsSnapshot` and its timestamp. On each scrape, if the cached snapshot is fresh enough (configurable TTL, e.g. 5s), it returns the cached copy without re-aggregating. A `?nocache=1` query param bypasses the cache. This prevents hammering the aggregation if a scraper misbehaves.

`MetricsHandler::onEOM()` sketch:
```cpp
void onEOM() noexcept override {
  folly::coro::co_invoke([this]() -> folly::coro::Task<void> {
    auto snapshot = co_await registry_->aggregateAsync();  // or cached
    auto text = StatsSnapshot::formatPrometheus(snapshot);
    ResponseBuilder(downstream_)
        .status(200, "OK")
        .header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
        .body(std::move(text))
        .sendWithEOM();
  })
  .scheduleOn(evb_)
  .start();
}
```

---

## StatsCollectorBase and StatsSnapshot

### X-macro pattern for StatsSnapshot

All scalar fields and all histograms are defined via X-macros. Adding a new counter, gauge, or histogram is **one line** in the corresponding macro; struct declaration, `operator+=`, and `formatPrometheus()` all follow automatically.

```cpp
// Scalars: uint64_t for counts (can't be negative), int64_t for gauges (negative = bug)
#define STATS_COUNTER_FIELDS(X)           \
  X(uint64_t, moqSubscribeSuccess)        \
  X(uint64_t, moqSubscribeError)          \
  X(uint64_t, moqFetchSuccess)            \
  X(uint64_t, moqFetchError)              \
  X(uint64_t, moqPublishNamespaceSuccess) \
  X(uint64_t, moqPublishNamespaceError)   \
  X(uint64_t, quicPacketsReceived)        \
  X(uint64_t, quicPacketsSent)            \
  X(uint64_t, quicPacketsDropped)         \
  X(uint64_t, quicPacketLoss)             \
  X(uint64_t, quicConnectionsCreated)     \
  X(uint64_t, quicConnectionsClosed)

#define STATS_GAUGE_FIELDS(X)             \
  X(int64_t, moqActiveSubscriptions)      \
  X(int64_t, moqActiveSessions)

// Histograms: (name, constexpr_boundaries_ref)
// Each expands to: name##Buckets[] + name##Sum + name##Count
inline constexpr std::array<uint64_t, 8> kLatencyBucketsMs = {1, 5, 10, 50, 100, 500, 1000, 5000};
#define STATS_HISTOGRAM_FIELDS(X)              \
  X(moqSubscribeLatency, kLatencyBucketsMs)    \
  X(moqFetchLatency,     kLatencyBucketsMs)

struct StatsSnapshot {
  // Scalar fields
#define DEFINE_FIELD(type, name) type name{0};
  STATS_COUNTER_FIELDS(DEFINE_FIELD)
  STATS_GAUGE_FIELDS(DEFINE_FIELD)
#undef DEFINE_FIELD

  // Histogram fields: bucket array (+Inf bucket), sum, count
#define DEFINE_HISTOGRAM(name, bounds) \
  std::array<uint64_t, std::tuple_size_v<decltype(bounds)> + 1> name##Buckets{}; \
  uint64_t name##Sum{0}; \
  uint64_t name##Count{0};
  STATS_HISTOGRAM_FIELDS(DEFINE_HISTOGRAM)
#undef DEFINE_HISTOGRAM

  StatsSnapshot& operator+=(const StatsSnapshot& o) {
#define ADD_FIELD(type, name) name += o.name;
    STATS_COUNTER_FIELDS(ADD_FIELD)
    STATS_GAUGE_FIELDS(ADD_FIELD)
#undef ADD_FIELD
#define ADD_HISTOGRAM(name, bounds) \
    for (size_t i = 0; i < name##Buckets.size(); ++i) name##Buckets[i] += o.name##Buckets[i]; \
    name##Sum += o.name##Sum; name##Count += o.name##Count;
    STATS_HISTOGRAM_FIELDS(ADD_HISTOGRAM)
#undef ADD_HISTOGRAM
    return *this;
  }

  static std::string formatPrometheus(const StatsSnapshot&);
  // formatPrometheus uses STATS_COUNTER_FIELDS, STATS_GAUGE_FIELDS, STATS_HISTOGRAM_FIELDS
  // to emit # HELP / # TYPE / value lines; histogram emits _bucket{le=...}/_sum/_count
};
```

### StatsCollectorBase
```cpp
class StatsCollectorBase {
 public:
  virtual ~StatsCollectorBase() = default;
  // Must be called on owningExecutor()
  virtual StatsSnapshot snapshot() const = 0;
  virtual folly::Executor::KeepAlive<> owningExecutor() const = 0;
};
```

**Note on `moqActiveSessions`:** `+1` in `onNewSession()`, `-1` in `terminateClientSession()` covers server-side sessions. If o-rly later acts as a client (upstream relay connection), those sessions have a different lifecycle — a separate counter or hook will be needed at that point.

---

## Phase 0: AdminServer Framework

**Thesis:** Establish a minimal, standalone proxygen HTTP/1.1+H2 admin server with a generic request aggregator. No stats yet — just the framework that later phases build on.

### Core design: generic request aggregator

Route handlers are simple callbacks rather than full `proxygen::RequestHandler` subclasses. A single `AdminRequestHandler` accumulates the complete request (headers + body) and then dispatches:

```cpp
// Route handler signature: receives complete request, owns the response
using RouteHandler = std::function<void(
    std::unique_ptr<proxygen::HTTPMessage> req,  // headers, path, query params, method
    std::unique_ptr<folly::IOBuf> body,          // complete body (may be empty)
    proxygen::ResponseHandler* downstream        // send headers/body/EOM here
)>;

// Generic aggregator — the only proxygen::RequestHandler subclass needed
class AdminRequestHandler : public proxygen::RequestHandler {
  RouteHandler handler_;
  std::unique_ptr<proxygen::HTTPMessage> req_;
  folly::IOBufQueue body_{folly::IOBufQueue::cacheChainLength()};
 public:
  explicit AdminRequestHandler(RouteHandler handler);
  void onRequest(std::unique_ptr<proxygen::HTTPMessage>) noexcept override;
  void onBody(std::unique_ptr<folly::IOBuf>) noexcept override;  // appends to body_
  void onEOM() noexcept override;  // calls handler_(req_, body_.move(), downstream_)
  void onUpgrade(proxygen::UpgradeProtocol) noexcept override {}
  void requestComplete() noexcept override { delete this; }
  void onError(proxygen::ProxygenError) noexcept override { delete this; }
};
```

`AdminHandlerFactory : proxygen::RequestHandlerFactory` holds a route table (method + path → `RouteHandler`); `onRequest()` looks up the route and returns an `AdminRequestHandler`. Unknown routes get a 404 handler.

Async handlers (e.g., MetricsHandler that needs `co_await`) launch a coroutine from within the `RouteHandler` and send the response asynchronously via `downstream`:

```cpp
// Registering the metrics route (sketch)
adminServer.addRoute("GET", "/metrics", [this](auto req, auto body, auto* downstream) {
  folly::coro::co_invoke([this, downstream]() -> folly::coro::Task<void> {
    auto snapshot = co_await registry_.aggregateAsync();
    auto text = StatsSnapshot::formatPrometheus(snapshot);
    ResponseBuilder(downstream).status(200, "OK")
        .header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
        .body(folly::IOBuf::copyBuffer(text)).sendWithEOM();
  }).scheduleOn(evb_).start();
});
```

### Files to Create
- `include/o_rly/admin/AdminServer.h` / `src/admin/AdminServer.cpp`
  - `AdminRequestHandler` (aggregator, as above)
  - `AdminHandlerFactory : proxygen::RequestHandlerFactory`
  - `AdminServer`: wraps `proxygen::HTTPServer` with `IOThreadPoolExecutor(1)`;  `addRoute(method, path, RouteHandler)`; `start(port)` / `stop()`
  - Comments note: TLS/mTLS via `wangle::SSLContextConfig` (future, PR #29 config); future routes like `GET /sessions` for inspection
  - **Code comment:** Uses `proxygen/httpserver/HTTPServer.h`; future migration to `proxygen/lib/http/coro/server/HTTPServer.h`

### Files to Modify
- `src/main.cpp`
  - Add `DEFINE_int32(admin_port, 9669, "HTTP admin port (0 to disable)")`
  - Create `AdminServer`, register routes, call `start()` before `evb.loopForever()`; `admin_port=0` skips it
  - First route registered: `GET /info` → lambda returning JSON version string (validates the framework)
  - Addresses the existing `TODO: health checks / admin endpoints` comment
- `CMakeLists.txt` — add `proxygen::proxygenhttpserver`; `src/admin/AdminServer.cpp` to `o_rly_core`

### Files to Modify
- `src/main.cpp`
  - Add `DEFINE_int32(admin_port, 9669, "HTTP admin port (0 to disable)")`
  - Create `AdminServer`, start before `evb.loopForever()`; `admin_port=0` skips it
  - Addresses the existing `TODO: health checks / admin endpoints` comment
- `CMakeLists.txt` — add `proxygen::proxygenhttpserver`; new `src/admin/*.cpp` to `o_rly_core`

---

## Phase 1: Stats Registry + MoQ Stats + Latency Histogram (POC)

**Thesis:** Implement `StatsRegistry` with folly::coro aggregation, `MoQStatsCollector` for MoQ-level metrics, and a `folly::Histogram<int64_t>`-backed latency histogram; expose via `GET /metrics`.

### Files to Create
- `include/o_rly/stats/StatsRegistry.h` / `src/stats/StatsRegistry.cpp`
  - `StatsCollectorBase` interface (see above)
  - `StatsSnapshot` struct with X-macro fields + histogram bucket arrays (see above)
  - `StatsRegistry`: mutex-protected collector list; `aggregateAsync() -> folly::coro::Task<StatsSnapshot>`; snapshot cache with TTL + `?nocache=1` bypass
- `include/o_rly/stats/MoQStatsCollector.h` / `src/stats/MoQStatsCollector.cpp`
  - Implements `MoQPublisherStatsCallback` + `MoQSubscriberStatsCallback` + `StatsCollectorBase`
  - Plain counters (`uint64_t` / `int64_t`); `folly::Histogram<int64_t>` for subscribe and fetch latency
  - Registers with `StatsRegistry` on construction, deregisters on destruction
  - `snapshot()`: copies plain counters + copies histogram bucket cumulative counts into `StatsSnapshot`
- `src/admin/MetricsHandler.cpp` (no separate header needed)
  - A `RouteHandler` function (or lambda) registered with `AdminServer` for `GET /metrics`
  - Uses `co_await registry_.aggregateAsync()` inside a coroutine launched from the handler; sends Prometheus text response via `downstream`
  - No `proxygen::RequestHandler` subclass — the aggregator framework handles that

### Files to Modify
- `include/o_rly/ORelayServer.h` — add `std::shared_ptr<StatsRegistry> statsRegistry_`
- `src/ORelayServer.cpp`
  - `onNewSession()`: create `MoQStatsCollector` per session, call `session->setPublisherStatsCallback()` + `setSubscriberStatsCallback()`
  - `terminateClientSession()`: decrement `moqActiveSessions` before session goes away
- `CMakeLists.txt` — add new `src/stats/*.cpp`, `src/admin/MetricsHandler.cpp`

### Latency Histogram POC
`MoQStatsCollector` holds one `folly::Histogram<int64_t>` per histogram defined in `STATS_HISTOGRAM_FIELDS`. In `recordSubscribeLatency(uint64_t latencyMs)`: `subscribeLatencyHistogram_.addValue(latencyMs)` (on owning executor, no sync needed). In `snapshot()`: copy cumulative bucket counts from each `folly::Histogram` into the corresponding `name##Buckets` array in `StatsSnapshot` — also fill `name##Sum` and `name##Count`. `formatPrometheus()` uses `STATS_HISTOGRAM_FIELDS` macro to emit `_bucket{le=...}`, `_sum`, `_count` lines for each histogram. Adding histogram #3 is one line in `STATS_HISTOGRAM_FIELDS` and one `folly::Histogram` member in the collector.

---

## Phase 2: QUIC Transport Stats

**Thesis:** Implement `QuicStatsCollector` per QUIC worker; plug into `StatsRegistry`.

### Files to Create
- `include/o_rly/stats/QuicStatsCollector.h` / `src/stats/QuicStatsCollector.cpp`
  - `QuicStatsCollector : quic::QuicTransportStatsCallback + StatsCollectorBase`
    - Plain `uint64_t` counters; `owningExecutor()` captures worker's executor at construction (via `folly::getEventBase()` / keep-alive at factory `make()` time)
    - Overrides only the 6 methods in Stats Tracked; remaining ~44 are default no-ops
    - Registers/deregisters with `StatsRegistry`
  - `QuicStatsCollectorFactory : quic::QuicTransportStatsCallbackFactory`
    - `make()` creates a `QuicStatsCollector` pointing at the shared `StatsRegistry`

### Files to Modify
- `src/ORelayServer.cpp` — call `setQuicStatsFactory(std::make_unique<QuicStatsCollectorFactory>(statsRegistry_))` in constructor
- `CMakeLists.txt` — verify `mvfst::mvfst_transport` linked (likely transitive via moxygen)

### Stats Tracked
- `quicPacketsReceived`, `quicPacketsSent`, `quicPacketsDropped`, `quicPacketLoss`, `quicConnectionsCreated`, `quicConnectionsClosed`

**TODO:** Count QUIC-direct vs WebTransport sessions separately — needs a moxygen hook to distinguish `createMoQQuicSession()` vs WebTransport `Handler` path.

---

## CMakeLists.txt Changes Summary

| Phase | New sources | New link targets |
|-------|------------|-----------------|
| 0 | `src/admin/AdminServer.cpp`, `InfoHandler.cpp` | `proxygen::proxygenhttpserver` |
| 1 | `src/stats/StatsRegistry.cpp`, `MoQStatsCollector.cpp`, `src/admin/MetricsHandler.cpp` | likely transitive |
| 2 | `src/stats/QuicStatsCollector.cpp` | verify `mvfst::mvfst_transport` |

---

## Verification

- **Phase 0:** `curl http://localhost:9669/info` → JSON; `curl .../other` → 404; `--admin_port=0` → clean start
- **Phase 1:** Connect MoQ client; `curl .../metrics` → Prometheus text with counters and histogram buckets; reconnect → counters increment; `?nocache=1` bypasses cache
- **Phase 2:** Establish QUIC connection; `curl .../metrics` → `quic_connections_created_total` increments
