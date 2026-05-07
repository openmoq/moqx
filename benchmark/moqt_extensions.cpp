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

void BM_ExtensionsSerialize(unsigned iters, int n) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto exts = makeExtensions(n);
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    folly::IOBufQueue buf;
    size_t sz = 0;
    bool err = false;
    writer.writeExtensions(buf, exts, sz, err);
    folly::doNotOptimizeAway(sz);
  }
}
BENCHMARK_NAMED_PARAM(BM_ExtensionsSerialize, _1, 1)
BENCHMARK_NAMED_PARAM(BM_ExtensionsSerialize, _10, 10)
BENCHMARK_NAMED_PARAM(BM_ExtensionsSerialize, _100, 100)
BENCHMARK_NAMED_PARAM(BM_ExtensionsSerialize, _1000, 1000)

// --- Serialize with array values ---

void BM_ExtensionsSerializeArray(unsigned iters, int n) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto exts = makeArrayExtensions(n);
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    folly::IOBufQueue buf;
    size_t sz = 0;
    bool err = false;
    writer.writeExtensions(buf, exts, sz, err);
    folly::doNotOptimizeAway(sz);
  }
}
BENCHMARK_NAMED_PARAM(BM_ExtensionsSerializeArray, _1, 1)
BENCHMARK_NAMED_PARAM(BM_ExtensionsSerializeArray, _10, 10)
BENCHMARK_NAMED_PARAM(BM_ExtensionsSerializeArray, _100, 100)

// --- Deserialize only (compare to libquicr ExtensionsDeserialize/N) ---

void BM_ExtensionsDeserialize(unsigned iters, int n) {
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
}
BENCHMARK_NAMED_PARAM(BM_ExtensionsDeserialize, _1, 1)
BENCHMARK_NAMED_PARAM(BM_ExtensionsDeserialize, _10, 10)
BENCHMARK_NAMED_PARAM(BM_ExtensionsDeserialize, _100, 100)
BENCHMARK_NAMED_PARAM(BM_ExtensionsDeserialize, _1000, 1000)

// --- Roundtrip: serialize then parse (compare to libquicr ExtensionsRoundTrip/N) ---

void BM_ExtensionsRoundTrip(unsigned iters, int n) {
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
}
BENCHMARK_NAMED_PARAM(BM_ExtensionsRoundTrip, _1, 1)
BENCHMARK_NAMED_PARAM(BM_ExtensionsRoundTrip, _10, 10)
BENCHMARK_NAMED_PARAM(BM_ExtensionsRoundTrip, _100, 100)
BENCHMARK_NAMED_PARAM(BM_ExtensionsRoundTrip, _1000, 1000)

} // namespace
