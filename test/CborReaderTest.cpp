/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/CborReader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

using openmoq::moqx::auth::CborReader;

namespace {

// Encode a CBOR data item head (major type + argument) using the minimal-width
// integer encoding, mirroring how real encoders lay out the wire bytes.
void putHead(std::string& out, uint8_t major, uint64_t value) {
  const uint8_t hi = static_cast<uint8_t>(major << 5);
  if (value < 24) {
    out.push_back(static_cast<char>(hi | value));
  } else if (value <= 0xff) {
    out.push_back(static_cast<char>(hi | 24));
    out.push_back(static_cast<char>(value));
  } else if (value <= 0xffff) {
    out.push_back(static_cast<char>(hi | 25));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
  } else if (value <= 0xffffffffULL) {
    out.push_back(static_cast<char>(hi | 26));
    for (int shift = 24; shift >= 0; shift -= 8) {
      out.push_back(static_cast<char>((value >> shift) & 0xff));
    }
  } else {
    out.push_back(static_cast<char>(hi | 27));
    for (int shift = 56; shift >= 0; shift -= 8) {
      out.push_back(static_cast<char>((value >> shift) & 0xff));
    }
  }
}

std::string uint64Cbor(uint64_t value) {
  std::string out;
  putHead(out, 0, value);
  return out;
}

} // namespace

TEST(CborReaderTest, ReadUIntDecodesAllArgumentWidths) {
  const uint64_t values[] = {
      0,
      23,
      24,
      0xff,
      0x100,
      0xffff,
      0x10000,
      0xffffffffULL,
      0x100000000ULL,
  };
  for (uint64_t v : values) {
    CborReader reader(uint64Cbor(v));
    uint64_t out = 1;
    ASSERT_TRUE(reader.readUInt(out)) << "value=" << v;
    EXPECT_EQ(out, v);
    EXPECT_TRUE(reader.eof());
  }
}

TEST(CborReaderTest, ReadUIntRejectsNonUnsignedMajorType) {
  std::string neg;
  putHead(neg, 1, 5); // negative integer
  CborReader reader(neg);
  uint64_t out = 0;
  EXPECT_FALSE(reader.readUInt(out));
}

TEST(CborReaderTest, ReadIntDecodesPositiveAndNegative) {
  {
    CborReader reader(uint64Cbor(5));
    int64_t out = 0;
    ASSERT_TRUE(reader.readInt(out));
    EXPECT_EQ(out, 5);
  }
  {
    std::string neg;
    putHead(neg, 1, 0); // -1 - 0 = -1
    CborReader reader(neg);
    int64_t out = 0;
    ASSERT_TRUE(reader.readInt(out));
    EXPECT_EQ(out, -1);
  }
  {
    std::string neg;
    putHead(neg, 1, 9); // -1 - 9 = -10
    CborReader reader(neg);
    int64_t out = 0;
    ASSERT_TRUE(reader.readInt(out));
    EXPECT_EQ(out, -10);
  }
}

TEST(CborReaderTest, ReadIntRejectsValuesThatOverflowInt64) {
  const uint64_t tooBig = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;
  {
    std::string pos;
    putHead(pos, 0, tooBig);
    CborReader reader(pos);
    int64_t out = 0;
    EXPECT_FALSE(reader.readInt(out));
  }
  {
    std::string neg;
    putHead(neg, 1, tooBig);
    CborReader reader(neg);
    int64_t out = 0;
    EXPECT_FALSE(reader.readInt(out));
  }
}

TEST(CborReaderTest, ReadBytesHandlesByteAndTextStrings) {
  for (uint8_t major : {uint8_t{2}, uint8_t{3}}) {
    std::string data;
    putHead(data, major, 3);
    data.append("abc");
    CborReader reader(data);
    std::string out;
    ASSERT_TRUE(reader.readBytes(out)) << "major=" << int(major);
    EXPECT_EQ(out, "abc");
    EXPECT_TRUE(reader.eof());
  }
}

TEST(CborReaderTest, ReadBytesRejectsLengthPastEndOfBuffer) {
  std::string data;
  putHead(data, 2, 5); // claims 5 bytes...
  data.append("ab");   // ...but only 2 are present
  CborReader reader(data);
  std::string out;
  EXPECT_FALSE(reader.readBytes(out));
}

TEST(CborReaderTest, ReadArrayAndMapLengths) {
  {
    std::string arr;
    putHead(arr, 4, 7);
    CborReader reader(arr);
    uint64_t len = 0;
    ASSERT_TRUE(reader.readArrayLen(len));
    EXPECT_EQ(len, 7u);
    EXPECT_FALSE(reader.readMapLen(len)); // nothing left, and wrong major anyway
  }
  {
    std::string map;
    putHead(map, 5, 2);
    CborReader reader(map);
    uint64_t len = 0;
    ASSERT_TRUE(reader.readMapLen(len));
    EXPECT_EQ(len, 2u);
  }
}

TEST(CborReaderTest, SkipTraversesNestedContainers) {
  // [ 1, "hi", { 2: 3 } ]
  std::string data;
  putHead(data, 4, 3);
  putHead(data, 0, 1);
  putHead(data, 3, 2);
  data.append("hi");
  putHead(data, 5, 1);
  putHead(data, 0, 2);
  putHead(data, 0, 3);
  // Trailing sentinel so we can confirm skip() consumed exactly the array.
  putHead(data, 0, 99);

  CborReader reader(data);
  ASSERT_TRUE(reader.skip());
  uint64_t sentinel = 0;
  ASSERT_TRUE(reader.readUInt(sentinel));
  EXPECT_EQ(sentinel, 99u);
  EXPECT_TRUE(reader.eof());
}

TEST(CborReaderTest, RejectsTruncatedMultiByteHeader) {
  std::string data;
  data.push_back(static_cast<char>((0 << 5) | 25)); // major 0, expects 2 arg bytes
  data.push_back(static_cast<char>(0x01));          // only 1 present
  CborReader reader(data);
  uint64_t out = 0;
  EXPECT_FALSE(reader.readUInt(out));
}

TEST(CborReaderTest, EofReflectsConsumption) {
  CborReader empty(std::string_view{});
  EXPECT_TRUE(empty.eof());

  CborReader reader(uint64Cbor(42));
  EXPECT_FALSE(reader.eof());
  uint64_t out = 0;
  ASSERT_TRUE(reader.readUInt(out));
  EXPECT_TRUE(reader.eof());
}
