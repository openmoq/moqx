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
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    auto buf = wireData->clone();
    folly::io::Cursor cursor(buf.get());
    ObjectHeader header;
    header.extensions = Extensions();
    size_t length = buf->computeChainDataLength();
    auto res = parser.parseExtensions(cursor, length, header);
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

  for (unsigned i = 0; i < iters; ++i) {
    // Parse
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    auto buf = wireData->clone();
    folly::io::Cursor cursor(buf.get());
    ObjectHeader header;
    header.extensions = Extensions();
    size_t length = buf->computeChainDataLength();
    auto res = parser.parseExtensions(cursor, length, header);
    folly::doNotOptimizeAway(res);

    // Reserialize
    folly::IOBufQueue outBuf;
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

} // namespace
