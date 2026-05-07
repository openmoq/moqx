#include <folly/Benchmark.h>
#include <moxygen/MoQFramer.h>
#include <moxygen/MoQTypes.h>

namespace {

using namespace moxygen;

constexpr uint64_t kVersion = kVersionDraftCurrent;

// Build Extensions with N mutable int-type extensions.
// Mirrors libquicr's ExtensionsSerialize/N benchmark which uses N extensions.
static Extensions makeExtensions(int count) {
  std::vector<Extension> exts;
  exts.reserve(count);
  for (int i = 0; i < count; ++i) {
    // Even type = int value (like libquicr's benchmark)
    exts.emplace_back(static_cast<uint64_t>(i * 2), static_cast<uint64_t>(i * 1000 + 42));
  }
  return Extensions(exts, {});
}

// Build Extensions with N mutable array-type extensions (odd type).
static Extensions makeArrayExtensions(int count) {
  std::vector<Extension> exts;
  exts.reserve(count);
  for (int i = 0; i < count; ++i) {
    auto buf = folly::IOBuf::copyBuffer("extension-payload-data-here");
    exts.emplace_back(static_cast<uint64_t>(i * 2 + 1), std::move(buf));
  }
  return Extensions(exts, {});
}

// Build extensions with libquicr's CreateTestExtensions input shape:
// N entries with type = typeBase + i and 8-byte payload value = type.
// Even types -> int form (varint value), odd types -> array form (8-byte IOBuf
// payload). Matches libquicr's per-extension data shape on the input side;
// the on-wire encoding still respects moq's type-parity rule, so even-type
// entries skip the memcpy that libquicr does internally for byte-array payloads.
// (The "_LikeLibquicrAllArray" variant below forces array form everywhere
// and pulls the encoder into the same memcpy work libquicr does.)
static std::vector<Extension> makeLibquicrShapeOne(int count, uint64_t typeBase) {
  std::vector<Extension> result;
  result.reserve(count);
  for (int i = 0; i < count; ++i) {
    uint64_t type = typeBase + static_cast<uint64_t>(i);
    uint64_t value = type;
    if (type % 2 == 0) {
      // Even type -> int form (no payload memcpy in encoder; varint-encoded value)
      result.emplace_back(type, value);
    } else {
      // Odd type -> array form (8-byte IOBuf, encoder memcpy's payload to wire)
      auto buf = folly::IOBuf::create(sizeof(value));
      buf->append(sizeof(value));
      std::memcpy(buf->writableData(), &value, sizeof(value));
      result.emplace_back(type, std::move(buf));
    }
  }
  return result;
}

// All-array variant: every entry odd-typed, 8-byte IOBuf payload. Forces the
// moxygen encoder to do the same byte-array memcpy work libquicr's
// SerializeExtensions does for every extension. Apples-to-apples on encoder cost.
static std::vector<Extension> makeAllArrayShapeOne(int count, uint64_t typeBase) {
  std::vector<Extension> result;
  result.reserve(count);
  for (int i = 0; i < count; ++i) {
    // Force odd type by stepping by 2 from a known-odd base
    uint64_t type = (typeBase | 1ull) + static_cast<uint64_t>(i) * 2ull;
    uint64_t value = type;
    auto buf = folly::IOBuf::create(sizeof(value));
    buf->append(sizeof(value));
    std::memcpy(buf->writableData(), &value, sizeof(value));
    result.emplace_back(type, std::move(buf));
  }
  return result;
}

// --- Serialize benchmarks (compare to libquicr ExtensionsSerialize/N) ---
//
// All four families below report bytes_per_iter so apples-to-apples
// throughput (bytes/sec = bytes_per_iter / ns_per_iter * 1e9) can be
// computed regardless of benchmark framework — folly here vs Google
// Benchmark on the libquicr side. The wire byte count is the natural
// normalization point and removes per-iteration framework overhead
// from the comparison.

void BM_ExtensionsSerialize(folly::UserCounters& counters, unsigned iters, int n) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto exts = makeExtensions(n);
  susp.dismiss();
  size_t totalBytes = 0;
  for (unsigned i = 0; i < iters; ++i) {
    folly::IOBufQueue buf;
    size_t sz = 0;
    bool err = false;
    writer.writeExtensions(buf, exts, sz, err);
    folly::doNotOptimizeAway(sz);
    totalBytes += sz;
  }
  counters["bytes_per_iter"] = iters > 0 ? totalBytes / iters : 0;
}
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsSerialize, _1, 1)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsSerialize, _10, 10)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsSerialize, _100, 100)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsSerialize, _1000, 1000)

// --- Serialize with array values ---

void BM_ExtensionsSerializeArray(folly::UserCounters& counters, unsigned iters, int n) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto exts = makeArrayExtensions(n);
  susp.dismiss();
  size_t totalBytes = 0;
  for (unsigned i = 0; i < iters; ++i) {
    folly::IOBufQueue buf;
    size_t sz = 0;
    bool err = false;
    writer.writeExtensions(buf, exts, sz, err);
    folly::doNotOptimizeAway(sz);
    totalBytes += sz;
  }
  counters["bytes_per_iter"] = iters > 0 ? totalBytes / iters : 0;
}
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsSerializeArray, _1, 1)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsSerializeArray, _10, 10)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsSerializeArray, _100, 100)

// --- Deserialize only (compare to libquicr ExtensionsDeserialize/N) ---

void BM_ExtensionsDeserialize(folly::UserCounters& counters, unsigned iters, int n) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto exts = makeExtensions(n);

  // Serialize once to get wire format
  folly::IOBufQueue writeBuf;
  size_t sz = 0;
  bool err = false;
  writer.writeExtensions(writeBuf, exts, sz, err);
  auto wireData = writeBuf.move();
  size_t wireSize = wireData->computeChainDataLength();
  susp.dismiss();

  for (unsigned i = 0; i < iters; ++i) {
    // Parser is per-iter: it carries state that persists across calls and
    // would short-circuit subsequent parseExtensions on the same wireData.
    // Cursor is hoisted off a fresh re-bind to wireData each iter (no clone
    // needed — Cursor is read-only over the IOBuf chain).
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    folly::io::Cursor cursor(wireData.get());
    ObjectHeader header;
    header.extensions = Extensions();
    auto res = parser.parseExtensions(cursor, wireSize, header);
    folly::doNotOptimizeAway(res);
  }
  counters["bytes_per_iter"] = wireSize;
}
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsDeserialize, _1, 1)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsDeserialize, _10, 10)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsDeserialize, _100, 100)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsDeserialize, _1000, 1000)

// --- Roundtrip: serialize then parse (compare to libquicr ExtensionsRoundTrip/N) ---

void BM_ExtensionsRoundTrip(folly::UserCounters& counters, unsigned iters, int n) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto exts = makeExtensions(n);

  // Serialize once to get wire format
  folly::IOBufQueue writeBuf;
  size_t sz = 0;
  bool err = false;
  writer.writeExtensions(writeBuf, exts, sz, err);
  auto wireData = writeBuf.move();
  size_t wireSize = wireData->computeChainDataLength();
  susp.dismiss();

  folly::IOBufQueue outBuf;
  for (unsigned i = 0; i < iters; ++i) {
    // Parser must be fresh per iter (carries state that would short-circuit
    // subsequent calls). Cursor reads wireData directly — no clone needed.
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    folly::io::Cursor cursor(wireData.get());
    ObjectHeader header;
    header.extensions = Extensions();
    auto res = parser.parseExtensions(cursor, wireSize, header);
    folly::doNotOptimizeAway(res);

    // Discard prior iter's output; cheaper than reconstructing the queue.
    outBuf.move();
    size_t outSz = 0;
    bool outErr = false;
    writer.writeExtensions(outBuf, header.extensions, outSz, outErr);
    folly::doNotOptimizeAway(outSz);
  }
  // Roundtrip processes the wire payload twice per iter (parse + reserialize),
  // so report the input wire size to keep the metric comparable with the
  // one-way Serialize/Deserialize benchmarks (consistent definition of
  // "bytes processed in a unit of wire").
  counters["bytes_per_iter"] = wireSize;
}
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsRoundTrip, _1, 1)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsRoundTrip, _10, 10)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsRoundTrip, _100, 100)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsRoundTrip, _1000, 1000)

// =============================================================================
// "Libquicr-shape" replica families
// =============================================================================
// These mirror libquicr's ExtensionsSerialize/Deserialize/RoundTrip benchmark
// shape so on-argo comparisons with libquicr's quicr_benchmark are direct:
//
// - 2N extensions per call: a "mutable" set of N + an "immutable" set of N,
//   matching libquicr's `SerializeExtensions(buffer, mutable_, immutable_)` call
//   shape. (libquicr's existing benchmark calls CreateTestExtensions twice.)
// - 8-byte payloads per extension, value = type, matching libquicr's
//   `Bytes(sizeof(uint64_t)); memcpy(...)` setup.
//
// Two replica families:
//
//   _LibquicrShape:  type values mixed parity (typeBase + i for both sets,
//                    different bases to avoid type collision across
//                    mutable/immutable). Even types use moxygen's int form
//                    (varint value, no payload memcpy in encoder); odd types
//                    use array form (8-byte IOBuf, encoder memcpy's payload).
//                    Matches libquicr's INPUT shape but encoder cost is
//                    asymmetric across the parities.
//
//   _LikeLibquicrAllArray:  every entry odd-typed and array-form with 8-byte
//                    IOBuf payload. Forces the moxygen encoder into the same
//                    per-extension memcpy work libquicr does for all entries.
//                    Apples-to-apples on encoder cost.

static Extensions buildLibquicrShape(int count) {
  return Extensions(
      makeLibquicrShapeOne(count, 1000),
      makeLibquicrShapeOne(count, 2000));
}

static Extensions buildAllArrayShape(int count) {
  return Extensions(
      makeAllArrayShapeOne(count, 1001),
      makeAllArrayShapeOne(count, 2001));
}

// --- Serialize replicas ---

void BM_ExtensionsSerialize_LibquicrShape(
    folly::UserCounters& counters, unsigned iters, int n) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto exts = buildLibquicrShape(n);
  susp.dismiss();
  size_t totalBytes = 0;
  for (unsigned i = 0; i < iters; ++i) {
    folly::IOBufQueue buf;
    size_t sz = 0;
    bool err = false;
    writer.writeExtensions(buf, exts, sz, err);
    folly::doNotOptimizeAway(sz);
    totalBytes += sz;
  }
  counters["bytes_per_iter"] = iters > 0 ? totalBytes / iters : 0;
  counters["exts_per_iter"] = static_cast<uint64_t>(n) * 2;
}
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsSerialize_LibquicrShape, _1, 1)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsSerialize_LibquicrShape, _10, 10)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsSerialize_LibquicrShape, _100, 100)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsSerialize_LibquicrShape, _1000, 1000)

void BM_ExtensionsSerialize_LikeLibquicrAllArray(
    folly::UserCounters& counters, unsigned iters, int n) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto exts = buildAllArrayShape(n);
  susp.dismiss();
  size_t totalBytes = 0;
  for (unsigned i = 0; i < iters; ++i) {
    folly::IOBufQueue buf;
    size_t sz = 0;
    bool err = false;
    writer.writeExtensions(buf, exts, sz, err);
    folly::doNotOptimizeAway(sz);
    totalBytes += sz;
  }
  counters["bytes_per_iter"] = iters > 0 ? totalBytes / iters : 0;
  counters["exts_per_iter"] = static_cast<uint64_t>(n) * 2;
}
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsSerialize_LikeLibquicrAllArray, _1, 1)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsSerialize_LikeLibquicrAllArray, _10, 10)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsSerialize_LikeLibquicrAllArray, _100, 100)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsSerialize_LikeLibquicrAllArray, _1000, 1000)

// --- Deserialize replicas ---

void BM_ExtensionsDeserialize_LibquicrShape(
    folly::UserCounters& counters, unsigned iters, int n) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto exts = buildLibquicrShape(n);
  folly::IOBufQueue writeBuf;
  size_t sz = 0;
  bool err = false;
  writer.writeExtensions(writeBuf, exts, sz, err);
  auto wireData = writeBuf.move();
  size_t wireSize = wireData->computeChainDataLength();
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    // Parser is per-iter: it carries state that persists across calls and
    // would short-circuit subsequent parseExtensions on the same wireData.
    // Cursor is hoisted off a fresh re-bind to wireData each iter (no clone
    // needed — Cursor is read-only over the IOBuf chain).
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    folly::io::Cursor cursor(wireData.get());
    ObjectHeader header;
    header.extensions = Extensions();
    auto res = parser.parseExtensions(cursor, wireSize, header);
    folly::doNotOptimizeAway(res);
  }
  counters["bytes_per_iter"] = wireSize;
  counters["exts_per_iter"] = static_cast<uint64_t>(n) * 2;
}
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsDeserialize_LibquicrShape, _1, 1)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsDeserialize_LibquicrShape, _10, 10)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsDeserialize_LibquicrShape, _100, 100)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsDeserialize_LibquicrShape, _1000, 1000)

void BM_ExtensionsDeserialize_LikeLibquicrAllArray(
    folly::UserCounters& counters, unsigned iters, int n) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto exts = buildAllArrayShape(n);
  folly::IOBufQueue writeBuf;
  size_t sz = 0;
  bool err = false;
  writer.writeExtensions(writeBuf, exts, sz, err);
  auto wireData = writeBuf.move();
  size_t wireSize = wireData->computeChainDataLength();
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    // Parser is per-iter: it carries state that persists across calls and
    // would short-circuit subsequent parseExtensions on the same wireData.
    // Cursor is hoisted off a fresh re-bind to wireData each iter (no clone
    // needed — Cursor is read-only over the IOBuf chain).
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    folly::io::Cursor cursor(wireData.get());
    ObjectHeader header;
    header.extensions = Extensions();
    auto res = parser.parseExtensions(cursor, wireSize, header);
    folly::doNotOptimizeAway(res);
  }
  counters["bytes_per_iter"] = wireSize;
  counters["exts_per_iter"] = static_cast<uint64_t>(n) * 2;
}
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsDeserialize_LikeLibquicrAllArray, _1, 1)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsDeserialize_LikeLibquicrAllArray, _10, 10)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsDeserialize_LikeLibquicrAllArray, _100, 100)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsDeserialize_LikeLibquicrAllArray, _1000, 1000)

// --- RoundTrip replicas ---

void BM_ExtensionsRoundTrip_LibquicrShape(
    folly::UserCounters& counters, unsigned iters, int n) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto exts = buildLibquicrShape(n);
  folly::IOBufQueue writeBuf;
  size_t sz = 0;
  bool err = false;
  writer.writeExtensions(writeBuf, exts, sz, err);
  auto wireData = writeBuf.move();
  size_t wireSize = wireData->computeChainDataLength();
  susp.dismiss();
  folly::IOBufQueue outBuf;
  for (unsigned i = 0; i < iters; ++i) {
    // Parser must be fresh per iter (carries state that would short-circuit
    // subsequent calls). Cursor reads wireData directly — no clone needed.
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    folly::io::Cursor cursor(wireData.get());
    ObjectHeader header;
    header.extensions = Extensions();
    auto res = parser.parseExtensions(cursor, wireSize, header);
    folly::doNotOptimizeAway(res);

    // Discard prior iter's output; cheaper than reconstructing the queue.
    outBuf.move();
    size_t outSz = 0;
    bool outErr = false;
    writer.writeExtensions(outBuf, header.extensions, outSz, outErr);
    folly::doNotOptimizeAway(outSz);
  }
  counters["bytes_per_iter"] = wireSize;
  counters["exts_per_iter"] = static_cast<uint64_t>(n) * 2;
}
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsRoundTrip_LibquicrShape, _1, 1)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsRoundTrip_LibquicrShape, _10, 10)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsRoundTrip_LibquicrShape, _100, 100)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsRoundTrip_LibquicrShape, _1000, 1000)

void BM_ExtensionsRoundTrip_LikeLibquicrAllArray(
    folly::UserCounters& counters, unsigned iters, int n) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto exts = buildAllArrayShape(n);
  folly::IOBufQueue writeBuf;
  size_t sz = 0;
  bool err = false;
  writer.writeExtensions(writeBuf, exts, sz, err);
  auto wireData = writeBuf.move();
  size_t wireSize = wireData->computeChainDataLength();
  susp.dismiss();
  folly::IOBufQueue outBuf;
  for (unsigned i = 0; i < iters; ++i) {
    // Parser must be fresh per iter (carries state that would short-circuit
    // subsequent calls). Cursor reads wireData directly — no clone needed.
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    folly::io::Cursor cursor(wireData.get());
    ObjectHeader header;
    header.extensions = Extensions();
    auto res = parser.parseExtensions(cursor, wireSize, header);
    folly::doNotOptimizeAway(res);

    // Discard prior iter's output; cheaper than reconstructing the queue.
    outBuf.move();
    size_t outSz = 0;
    bool outErr = false;
    writer.writeExtensions(outBuf, header.extensions, outSz, outErr);
    folly::doNotOptimizeAway(outSz);
  }
  counters["bytes_per_iter"] = wireSize;
  counters["exts_per_iter"] = static_cast<uint64_t>(n) * 2;
}
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsRoundTrip_LikeLibquicrAllArray, _1, 1)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsRoundTrip_LikeLibquicrAllArray, _10, 10)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsRoundTrip_LikeLibquicrAllArray, _100, 100)
BENCHMARK_COUNTERS_NAMED_PARAM(BM_ExtensionsRoundTrip_LikeLibquicrAllArray, _1000, 1000)

} // namespace
