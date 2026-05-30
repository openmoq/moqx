/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace openmoq::moqx::auth {

// CBOR (RFC 8949 §3) initial-byte encoding. The high 3 bits select the major
// type; the low 5 bits ("additional information") either hold the argument
// directly (< 24) or name how many big-endian bytes follow (24..27 => 1/2/4/8).
namespace cbor {
constexpr unsigned kMajorShift = 5;
constexpr uint8_t kAddlInfoMask = 0x1f;

constexpr uint8_t kMajorUnsigned = 0;   // unsigned integer
constexpr uint8_t kMajorNegative = 1;   // negative integer
constexpr uint8_t kMajorByteString = 2; // byte string
constexpr uint8_t kMajorTextString = 3; // text string
constexpr uint8_t kMajorArray = 4;      // array
constexpr uint8_t kMajorMap = 5;        // map

constexpr uint8_t kAddlInfoDirectMax = 24; // additional info < this is the value itself
constexpr uint8_t kAddlInfo1Byte = 24;
constexpr uint8_t kAddlInfo2Byte = 25;
constexpr uint8_t kAddlInfo4Byte = 26;
constexpr uint8_t kAddlInfo8Byte = 27;
} // namespace cbor

// Minimal CBOR (RFC 8949) decoder used to parse CAT token claims.
// Internal to the auth subsystem.
class CborReader {
public:
  explicit CborReader(std::string_view data) : data_(data) {}

  bool eof() const { return pos_ == data_.size(); }

  bool readUInt(uint64_t& out) {
    uint8_t major = 0;
    uint64_t value = 0;
    if (!readType(major, value) || major != cbor::kMajorUnsigned) {
      return false;
    }
    out = value;
    return true;
  }

  bool readInt(int64_t& out) {
    uint8_t major = 0;
    uint64_t value = 0;
    if (!readType(major, value)) {
      return false;
    }
    if (major == cbor::kMajorUnsigned) {
      if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return false;
      }
      out = static_cast<int64_t>(value);
      return true;
    }
    if (major == cbor::kMajorNegative) {
      if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return false;
      }
      out = -1 - static_cast<int64_t>(value);
      return true;
    }
    return false;
  }

  bool readBytes(std::string& out) {
    uint8_t major = 0;
    uint64_t len = 0;
    if (!readType(major, len) ||
        (major != cbor::kMajorByteString && major != cbor::kMajorTextString) ||
        len > data_.size() - pos_) {
      return false;
    }
    out.assign(data_.data() + pos_, static_cast<size_t>(len));
    pos_ += static_cast<size_t>(len);
    return true;
  }

  bool readArrayLen(uint64_t& len) {
    uint8_t major = 0;
    return readType(major, len) && major == cbor::kMajorArray;
  }

  bool readMapLen(uint64_t& len) {
    uint8_t major = 0;
    return readType(major, len) && major == cbor::kMajorMap;
  }

  bool skip() {
    uint8_t major = 0;
    uint64_t value = 0;
    if (!readType(major, value)) {
      return false;
    }
    switch (major) {
    case cbor::kMajorUnsigned:
    case cbor::kMajorNegative:
      return true;
    case cbor::kMajorByteString:
    case cbor::kMajorTextString:
      if (value > data_.size() - pos_) {
        return false;
      }
      pos_ += static_cast<size_t>(value);
      return true;
    case cbor::kMajorArray:
      for (uint64_t i = 0; i < value; ++i) {
        if (!skip()) {
          return false;
        }
      }
      return true;
    case cbor::kMajorMap:
      for (uint64_t i = 0; i < value * 2; ++i) {
        if (!skip()) {
          return false;
        }
      }
      return true;
    default:
      return false;
    }
  }

private:
  bool readType(uint8_t& major, uint64_t& value) {
    if (pos_ >= data_.size()) {
      return false;
    }
    const auto first = static_cast<unsigned char>(data_[pos_++]);
    major = first >> cbor::kMajorShift;
    const auto addl = first & cbor::kAddlInfoMask;
    if (addl < cbor::kAddlInfoDirectMax) {
      value = addl;
      return true;
    }
    size_t bytes = 0;
    if (addl == cbor::kAddlInfo1Byte) {
      bytes = 1;
    } else if (addl == cbor::kAddlInfo2Byte) {
      bytes = 2;
    } else if (addl == cbor::kAddlInfo4Byte) {
      bytes = 4;
    } else if (addl == cbor::kAddlInfo8Byte) {
      bytes = 8;
    } else {
      return false;
    }
    if (bytes > data_.size() - pos_) {
      return false;
    }
    value = 0;
    for (size_t i = 0; i < bytes; ++i) {
      value = (value << 8) | static_cast<unsigned char>(data_[pos_++]);
    }
    return true;
  }

  std::string_view data_;
  size_t pos_{0};
};

} // namespace openmoq::moqx::auth
