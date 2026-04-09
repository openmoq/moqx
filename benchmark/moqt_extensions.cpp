#include <benchmark/benchmark.h>
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

static void BM_ExtensionsSerialize(benchmark::State& state) {
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto exts = makeExtensions(state.range(0));
  for (auto _ : state) {
    folly::IOBufQueue buf;
    size_t sz = 0;
    bool err = false;
    writer.writeExtensions(buf, exts, sz, err);
    benchmark::DoNotOptimize(sz);
  }
}
BENCHMARK(BM_ExtensionsSerialize)->Arg(1)->Arg(10)->Arg(100)->Arg(1000);

// --- Serialize with array values ---

static void BM_ExtensionsSerializeArray(benchmark::State& state) {
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto exts = makeArrayExtensions(state.range(0));
  for (auto _ : state) {
    folly::IOBufQueue buf;
    size_t sz = 0;
    bool err = false;
    writer.writeExtensions(buf, exts, sz, err);
    benchmark::DoNotOptimize(sz);
  }
}
BENCHMARK(BM_ExtensionsSerializeArray)->Arg(1)->Arg(10)->Arg(100);

// --- Deserialize only (compare to libquicr ExtensionsDeserialize/N) ---

static void BM_ExtensionsDeserialize(benchmark::State& state) {
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto exts = makeExtensions(state.range(0));

  // Serialize once to get wire format
  folly::IOBufQueue writeBuf;
  size_t sz = 0;
  bool err = false;
  writer.writeExtensions(writeBuf, exts, sz, err);
  auto wireData = writeBuf.move();

  for (auto _ : state) {
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    auto buf = wireData->clone();
    folly::io::Cursor cursor(buf.get());
    ObjectHeader header;
    header.extensions = Extensions();
    size_t length = buf->computeChainDataLength();
    auto res = parser.parseExtensions(cursor, length, header);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_ExtensionsDeserialize)->Arg(1)->Arg(10)->Arg(100)->Arg(1000);

// --- Roundtrip: serialize then parse (compare to libquicr ExtensionsRoundTrip/N) ---

static void BM_ExtensionsRoundTrip(benchmark::State& state) {
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto exts = makeExtensions(state.range(0));

  // Serialize once to get wire format
  folly::IOBufQueue writeBuf;
  size_t sz = 0;
  bool err = false;
  writer.writeExtensions(writeBuf, exts, sz, err);
  auto wireData = writeBuf.move();

  for (auto _ : state) {
    // Parse
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    auto buf = wireData->clone();
    folly::io::Cursor cursor(buf.get());
    ObjectHeader header;
    header.extensions = Extensions();
    size_t length = buf->computeChainDataLength();
    auto res = parser.parseExtensions(cursor, length, header);
    benchmark::DoNotOptimize(res);

    // Reserialize
    folly::IOBufQueue outBuf;
    size_t outSz = 0;
    bool outErr = false;
    writer.writeExtensions(outBuf, header.extensions, outSz, outErr);
    benchmark::DoNotOptimize(outSz);
  }
}
BENCHMARK(BM_ExtensionsRoundTrip)->Arg(1)->Arg(10)->Arg(100)->Arg(1000);

} // namespace
