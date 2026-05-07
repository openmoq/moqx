#include <folly/Benchmark.h>
#include <moxygen/MoQFramer.h>
#include <moxygen/MoQTypes.h>
#include <quic/codec/QuicInteger.h>
#include <quic/common/ContiguousCursor.h>

namespace {

using namespace moxygen;

constexpr uint64_t kVersion = kVersionDraftCurrent;

static TrackNamespace makeTrackNamespace() {
  std::vector<std::string> ns = {"conference", "room42"};
  return TrackNamespace(std::move(ns));
}

static FullTrackName makeFullTrackName() {
  FullTrackName ftn;
  ftn.trackNamespace = makeTrackNamespace();
  ftn.trackName = "video";
  return ftn;
}

// --- Varint encode/decode (compare to libquicr UIntVar/Encode/Decode) ---

// Cold: new IOBufQueue per call (measures allocation overhead, not varint logic)
BENCHMARK(BM_VarintEncode_Cold, iters) {
  uint64_t val = 42;
  for (unsigned i = 0; i < iters; ++i) {
    folly::IOBufQueue q;
    size_t sz = 0;
    bool err = false;
    moxygen::writeVarint(q, val, sz, err);
    folly::doNotOptimizeAway(sz);
  }
}

// Warm: reuse IOBufQueue (reflects production — buffer is pre-allocated)
BENCHMARK(BM_VarintEncode_Warm, iters) {
  folly::BenchmarkSuspender susp;
  uint64_t val = 42;
  folly::IOBufQueue q;
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    size_t sz = 0;
    bool err = false;
    moxygen::writeVarint(q, val, sz, err);
    folly::doNotOptimizeAway(sz);
  }
}

// Warm large value
BENCHMARK(BM_VarintEncode_WarmLarge, iters) {
  folly::BenchmarkSuspender susp;
  uint64_t val = 1000000000;
  folly::IOBufQueue q;
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    size_t sz = 0;
    bool err = false;
    moxygen::writeVarint(q, val, sz, err);
    folly::doNotOptimizeAway(sz);
  }
}

BENCHMARK(BM_VarintDecode_Small, iters) {
  folly::BenchmarkSuspender susp;
  uint8_t buf[8] = {42, 0, 0, 0, 0, 0, 0, 0}; // 1-byte encoding of 42
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    quic::ContiguousReadCursor cursor(buf, sizeof(buf));
    auto res = quic::decodeQuicInteger(cursor);
    folly::doNotOptimizeAway(res);
  }
}

BENCHMARK(BM_VarintDecode_Large, iters) {
  folly::BenchmarkSuspender susp;
  // Pre-encode a large value
  folly::IOBufQueue q;
  size_t sz = 0;
  bool err = false;
  moxygen::writeVarint(q, 1000000000, sz, err);
  auto iobuf = q.move();
  uint8_t buf[8] = {};
  memcpy(buf, iobuf->data(), iobuf->length());
  size_t len = iobuf->length();
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    quic::ContiguousReadCursor cursor(buf, len);
    auto res = quic::decodeQuicInteger(cursor);
    folly::doNotOptimizeAway(res);
  }
}

// --- Write benchmarks ---

BENCHMARK(BM_WriteSubscribeRequest, iters) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto req = SubscribeRequest::make(makeFullTrackName());
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    folly::IOBufQueue buf;
    auto res = writer.writeSubscribeRequest(buf, req);
    folly::doNotOptimizeAway(res);
  }
}

BENCHMARK(BM_WriteSubgroupHeader, iters) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  ObjectHeader header;
  header.group = 100;
  header.subgroup = 0;
  header.id = 0;
  header.priority = 128;
  header.status = ObjectStatus::NORMAL;
  header.length = 1024;
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    folly::IOBufQueue buf;
    auto res = writer.writeSubgroupHeader(buf, TrackAlias(1), header);
    folly::doNotOptimizeAway(res);
  }
}

BENCHMARK(BM_WriteStreamObject, iters) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  ObjectHeader header;
  header.group = 100;
  header.subgroup = 0;
  header.id = 5;
  header.priority = 128;
  header.status = ObjectStatus::NORMAL;
  header.length = 1024;
  auto payload = folly::IOBuf::create(1024);
  payload->append(1024);
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    folly::IOBufQueue buf;
    auto res = writer.writeStreamObject(
        buf,
        getSubgroupStreamType(kVersion, SubgroupIDFormat::Present, false, false),
        header,
        payload->clone());
    folly::doNotOptimizeAway(res);
  }
}

BENCHMARK(BM_WritePublishNamespace, iters) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  PublishNamespace pubNs;
  pubNs.requestID = RequestID(1);
  pubNs.trackNamespace = makeTrackNamespace();
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    folly::IOBufQueue buf;
    auto res = writer.writePublishNamespace(buf, pubNs);
    folly::doNotOptimizeAway(res);
  }
}

BENCHMARK(BM_WriteFetch, iters) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  Fetch fetch(
      RequestID(1),
      makeFullTrackName(),
      AbsoluteLocation{0, 0},
      AbsoluteLocation{10, 0});
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    folly::IOBufQueue buf;
    auto res = writer.writeFetch(buf, fetch);
    folly::doNotOptimizeAway(res);
  }
}

BENCHMARK(BM_WriteGoaway, iters) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  Goaway goaway;
  goaway.newSessionUri = "https://relay.example.com/moq-relay";
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    folly::IOBufQueue buf;
    auto res = writer.writeGoaway(buf, goaway);
    folly::doNotOptimizeAway(res);
  }
}

// --- Parse benchmarks ---

// Helper: skip a QUIC varint in a folly::io::Cursor.
// Returns the decoded value.
static uint64_t skipVarint(folly::io::Cursor& cursor) {
  uint8_t first = cursor.read<uint8_t>();
  uint8_t lenBits = first >> 6;
  uint64_t val = first & 0x3f;
  for (int i = 0; i < (1 << lenBits) - 1; ++i) {
    val = (val << 8) | cursor.read<uint8_t>();
  }
  return val;
}

BENCHMARK(BM_ParseSubscribeRequest, iters) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto req = SubscribeRequest::make(makeFullTrackName());
  folly::IOBufQueue writeBuf;
  writer.writeSubscribeRequest(writeBuf, req);
  auto wireData = writeBuf.move();
  susp.dismiss();

  for (unsigned i = 0; i < iters; ++i) {
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    auto buf = wireData->clone();
    folly::io::Cursor cursor(buf.get());
    skipVarint(cursor); // frame type
    auto frameLen = skipVarint(cursor); // payload length
    auto res = parser.parseSubscribeRequest(cursor, frameLen);
    folly::doNotOptimizeAway(res);
  }
}

BENCHMARK(BM_ParseFetch, iters) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  Fetch fetch(
      RequestID(1),
      makeFullTrackName(),
      AbsoluteLocation{0, 0},
      AbsoluteLocation{10, 0});
  folly::IOBufQueue writeBuf;
  writer.writeFetch(writeBuf, fetch);
  auto wireData = writeBuf.move();
  susp.dismiss();

  for (unsigned i = 0; i < iters; ++i) {
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    auto buf = wireData->clone();
    folly::io::Cursor cursor(buf.get());
    skipVarint(cursor);
    auto frameLen = skipVarint(cursor);
    auto res = parser.parseFetch(cursor, frameLen);
    folly::doNotOptimizeAway(res);
  }
}

BENCHMARK(BM_ParsePublishNamespace, iters) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  PublishNamespace pubNs;
  pubNs.requestID = RequestID(1);
  pubNs.trackNamespace = makeTrackNamespace();
  folly::IOBufQueue writeBuf;
  writer.writePublishNamespace(writeBuf, pubNs);
  auto wireData = writeBuf.move();
  susp.dismiss();

  for (unsigned i = 0; i < iters; ++i) {
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    auto buf = wireData->clone();
    folly::io::Cursor cursor(buf.get());
    skipVarint(cursor);
    auto frameLen = skipVarint(cursor);
    auto res = parser.parsePublishNamespace(cursor, frameLen);
    folly::doNotOptimizeAway(res);
  }
}

BENCHMARK(BM_ParseGoaway, iters) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  Goaway goaway;
  goaway.newSessionUri = "https://relay.example.com/moq-relay";
  folly::IOBufQueue writeBuf;
  writer.writeGoaway(writeBuf, goaway);
  auto wireData = writeBuf.move();
  susp.dismiss();

  for (unsigned i = 0; i < iters; ++i) {
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    auto buf = wireData->clone();
    folly::io::Cursor cursor(buf.get());
    skipVarint(cursor);
    auto frameLen = skipVarint(cursor);
    auto res = parser.parseGoaway(cursor, frameLen);
    folly::doNotOptimizeAway(res);
  }
}

// --- Subscribe write+parse roundtrip ---

BENCHMARK(BM_SubscribeRoundTrip, iters) {
  folly::BenchmarkSuspender susp;
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto req = SubscribeRequest::make(makeFullTrackName());
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    // Write
    folly::IOBufQueue writeBuf;
    writer.writeSubscribeRequest(writeBuf, req);
    auto buf = writeBuf.move();

    // Parse
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    folly::io::Cursor cursor(buf.get());
    skipVarint(cursor);
    auto frameLen = skipVarint(cursor);
    auto res = parser.parseSubscribeRequest(cursor, frameLen);
    folly::doNotOptimizeAway(res);
  }
}

// --- TrackNamespace operations ---

BENCHMARK(BM_TrackNamespace_Construct, iters) {
  for (unsigned i = 0; i < iters; ++i) {
    std::vector<std::string> parts = {"conference", "room42", "alice", "video"};
    TrackNamespace ns(std::move(parts));
    folly::doNotOptimizeAway(ns);
  }
}

BENCHMARK(BM_TrackNamespace_PrefixMatch, iters) {
  folly::BenchmarkSuspender susp;
  std::vector<std::string> fp = {"conference", "room42", "alice", "video"};
  TrackNamespace full(std::move(fp));
  std::vector<std::string> pp = {"conference", "room42"};
  TrackNamespace prefix(std::move(pp));
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    auto result = full.startsWith(prefix);
    folly::doNotOptimizeAway(result);
  }
}

BENCHMARK(BM_TrackNamespace_Hash, iters) {
  folly::BenchmarkSuspender susp;
  std::vector<std::string> parts = {"conference", "room42", "alice", "video"};
  TrackNamespace ns(std::move(parts));
  TrackNamespace::hash hasher;
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    auto h = hasher(ns);
    folly::doNotOptimizeAway(h);
  }
}

BENCHMARK(BM_TrackNamespace_Describe, iters) {
  folly::BenchmarkSuspender susp;
  std::vector<std::string> parts = {"conference", "room42", "alice", "video"};
  TrackNamespace ns(std::move(parts));
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    auto s = ns.describe();
    folly::doNotOptimizeAway(s);
  }
}

BENCHMARK(BM_FullTrackName_Compare, iters) {
  folly::BenchmarkSuspender susp;
  auto a = makeFullTrackName();
  auto b = makeFullTrackName();
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    auto result = (a == b);
    folly::doNotOptimizeAway(result);
  }
}

BENCHMARK(BM_FullTrackName_Hash, iters) {
  folly::BenchmarkSuspender susp;
  auto ftn = makeFullTrackName();
  FullTrackName::hash hasher;
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    auto h = hasher(ftn);
    folly::doNotOptimizeAway(h);
  }
}

} // namespace
