#include <benchmark/benchmark.h>
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

void BM_VarintEncode_Small(benchmark::State& state) {
  uint64_t val = 42; // 1-byte varint
  for (auto _ : state) {
    folly::IOBufQueue q;
    size_t sz = 0;
    bool err = false;
    moxygen::writeVarint(q, val, sz, err);
    benchmark::DoNotOptimize(sz);
  }
}
BENCHMARK(BM_VarintEncode_Small);

void BM_VarintEncode_Large(benchmark::State& state) {
  uint64_t val = 1000000000; // 4-byte varint
  for (auto _ : state) {
    folly::IOBufQueue q;
    size_t sz = 0;
    bool err = false;
    moxygen::writeVarint(q, val, sz, err);
    benchmark::DoNotOptimize(sz);
  }
}
BENCHMARK(BM_VarintEncode_Large);

void BM_VarintDecode_Small(benchmark::State& state) {
  uint8_t buf[8] = {42, 0, 0, 0, 0, 0, 0, 0}; // 1-byte encoding of 42
  for (auto _ : state) {
    quic::ContiguousReadCursor cursor(buf, sizeof(buf));
    auto res = quic::decodeQuicInteger(cursor);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_VarintDecode_Small);

void BM_VarintDecode_Large(benchmark::State& state) {
  // Pre-encode a large value
  folly::IOBufQueue q;
  size_t sz = 0;
  bool err = false;
  moxygen::writeVarint(q, 1000000000, sz, err);
  auto iobuf = q.move();
  uint8_t buf[8] = {};
  memcpy(buf, iobuf->data(), iobuf->length());
  size_t len = iobuf->length();
  for (auto _ : state) {
    quic::ContiguousReadCursor cursor(buf, len);
    auto res = quic::decodeQuicInteger(cursor);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_VarintDecode_Large);

// --- Write benchmarks ---

void BM_WriteSubscribeRequest(benchmark::State& state) {
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto req = SubscribeRequest::make(makeFullTrackName());
  for (auto _ : state) {
    folly::IOBufQueue buf;
    auto res = writer.writeSubscribeRequest(buf, req);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_WriteSubscribeRequest);

void BM_WriteSubgroupHeader(benchmark::State& state) {
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  ObjectHeader header;
  header.group = 100;
  header.subgroup = 0;
  header.id = 0;
  header.priority = 128;
  header.status = ObjectStatus::NORMAL;
  header.length = 1024;
  for (auto _ : state) {
    folly::IOBufQueue buf;
    auto res = writer.writeSubgroupHeader(buf, TrackAlias(1), header);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_WriteSubgroupHeader);

void BM_WriteStreamObject(benchmark::State& state) {
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
  for (auto _ : state) {
    folly::IOBufQueue buf;
    auto res = writer.writeStreamObject(
        buf,
        getSubgroupStreamType(kVersion, SubgroupIDFormat::Present, false, false),
        header,
        payload->clone());
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_WriteStreamObject);

void BM_WritePublishNamespace(benchmark::State& state) {
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  PublishNamespace pubNs;
  pubNs.requestID = RequestID(1);
  pubNs.trackNamespace = makeTrackNamespace();
  for (auto _ : state) {
    folly::IOBufQueue buf;
    auto res = writer.writePublishNamespace(buf, pubNs);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_WritePublishNamespace);

void BM_WriteFetch(benchmark::State& state) {
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  Fetch fetch(
      RequestID(1),
      makeFullTrackName(),
      AbsoluteLocation{0, 0},
      AbsoluteLocation{10, 0});
  for (auto _ : state) {
    folly::IOBufQueue buf;
    auto res = writer.writeFetch(buf, fetch);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_WriteFetch);

void BM_WriteGoaway(benchmark::State& state) {
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  Goaway goaway;
  goaway.newSessionUri = "https://relay.example.com/moq-relay";
  for (auto _ : state) {
    folly::IOBufQueue buf;
    auto res = writer.writeGoaway(buf, goaway);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_WriteGoaway);

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

void BM_ParseSubscribeRequest(benchmark::State& state) {
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto req = SubscribeRequest::make(makeFullTrackName());
  folly::IOBufQueue writeBuf;
  writer.writeSubscribeRequest(writeBuf, req);
  auto wireData = writeBuf.move();

  for (auto _ : state) {
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    auto buf = wireData->clone();
    folly::io::Cursor cursor(buf.get());
    skipVarint(cursor); // frame type
    auto frameLen = skipVarint(cursor); // payload length
    auto res = parser.parseSubscribeRequest(cursor, frameLen);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_ParseSubscribeRequest);

void BM_ParseFetch(benchmark::State& state) {
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

  for (auto _ : state) {
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    auto buf = wireData->clone();
    folly::io::Cursor cursor(buf.get());
    skipVarint(cursor);
    auto frameLen = skipVarint(cursor);
    auto res = parser.parseFetch(cursor, frameLen);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_ParseFetch);

void BM_ParsePublishNamespace(benchmark::State& state) {
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  PublishNamespace pubNs;
  pubNs.requestID = RequestID(1);
  pubNs.trackNamespace = makeTrackNamespace();
  folly::IOBufQueue writeBuf;
  writer.writePublishNamespace(writeBuf, pubNs);
  auto wireData = writeBuf.move();

  for (auto _ : state) {
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    auto buf = wireData->clone();
    folly::io::Cursor cursor(buf.get());
    skipVarint(cursor);
    auto frameLen = skipVarint(cursor);
    auto res = parser.parsePublishNamespace(cursor, frameLen);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_ParsePublishNamespace);

void BM_ParseGoaway(benchmark::State& state) {
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  Goaway goaway;
  goaway.newSessionUri = "https://relay.example.com/moq-relay";
  folly::IOBufQueue writeBuf;
  writer.writeGoaway(writeBuf, goaway);
  auto wireData = writeBuf.move();

  for (auto _ : state) {
    MoQFrameParser parser;
    parser.initializeVersion(kVersion);
    auto buf = wireData->clone();
    folly::io::Cursor cursor(buf.get());
    skipVarint(cursor);
    auto frameLen = skipVarint(cursor);
    auto res = parser.parseGoaway(cursor, frameLen);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_ParseGoaway);

// --- Subscribe write+parse roundtrip ---

void BM_SubscribeRoundTrip(benchmark::State& state) {
  MoQFrameWriter writer;
  writer.initializeVersion(kVersion);
  auto req = SubscribeRequest::make(makeFullTrackName());
  for (auto _ : state) {
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
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_SubscribeRoundTrip);

// --- TrackNamespace operations ---

void BM_TrackNamespace_Construct(benchmark::State& state) {
  for (auto _ : state) {
    std::vector<std::string> parts = {"conference", "room42", "alice", "video"};
    TrackNamespace ns(std::move(parts));
    benchmark::DoNotOptimize(ns);
  }
}
BENCHMARK(BM_TrackNamespace_Construct);

void BM_TrackNamespace_PrefixMatch(benchmark::State& state) {
  std::vector<std::string> fp = {"conference", "room42", "alice", "video"};
  TrackNamespace full(std::move(fp));
  std::vector<std::string> pp = {"conference", "room42"};
  TrackNamespace prefix(std::move(pp));
  for (auto _ : state) {
    auto result = full.startsWith(prefix);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_TrackNamespace_PrefixMatch);

void BM_TrackNamespace_Hash(benchmark::State& state) {
  std::vector<std::string> parts = {"conference", "room42", "alice", "video"};
  TrackNamespace ns(std::move(parts));
  TrackNamespace::hash hasher;
  for (auto _ : state) {
    auto h = hasher(ns);
    benchmark::DoNotOptimize(h);
  }
}
BENCHMARK(BM_TrackNamespace_Hash);

void BM_TrackNamespace_Describe(benchmark::State& state) {
  std::vector<std::string> parts = {"conference", "room42", "alice", "video"};
  TrackNamespace ns(std::move(parts));
  for (auto _ : state) {
    auto s = ns.describe();
    benchmark::DoNotOptimize(s);
  }
}
BENCHMARK(BM_TrackNamespace_Describe);

void BM_FullTrackName_Compare(benchmark::State& state) {
  auto a = makeFullTrackName();
  auto b = makeFullTrackName();
  for (auto _ : state) {
    auto result = (a == b);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_FullTrackName_Compare);

void BM_FullTrackName_Hash(benchmark::State& state) {
  auto ftn = makeFullTrackName();
  FullTrackName::hash hasher;
  for (auto _ : state) {
    auto h = hasher(ftn);
    benchmark::DoNotOptimize(h);
  }
}
BENCHMARK(BM_FullTrackName_Hash);

} // namespace
