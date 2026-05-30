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

// Minimal CBOR (RFC 8949) decoder used to parse CAT token claims.
// Internal to the auth subsystem.
class CborReader {
public:
  explicit CborReader(std::string_view data) : data_(data) {}

  bool eof() const { return pos_ == data_.size(); }

  bool readUInt(uint64_t& out) {
    uint8_t major = 0;
    uint64_t value = 0;
    if (!readType(major, value) || major != 0) {
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
    if (major == 0) {
      if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return false;
      }
      out = static_cast<int64_t>(value);
      return true;
    }
    if (major == 1) {
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
    if (!readType(major, len) || (major != 2 && major != 3) || len > data_.size() - pos_) {
      return false;
    }
    out.assign(data_.data() + pos_, static_cast<size_t>(len));
    pos_ += static_cast<size_t>(len);
    return true;
  }

  bool readArrayLen(uint64_t& len) {
    uint8_t major = 0;
    return readType(major, len) && major == 4;
  }

  bool readMapLen(uint64_t& len) {
    uint8_t major = 0;
    return readType(major, len) && major == 5;
  }

  bool skip() {
    uint8_t major = 0;
    uint64_t value = 0;
    if (!readType(major, value)) {
      return false;
    }
    switch (major) {
    case 0:
    case 1:
      return true;
    case 2:
    case 3:
      if (value > data_.size() - pos_) {
        return false;
      }
      pos_ += static_cast<size_t>(value);
      return true;
    case 4:
      for (uint64_t i = 0; i < value; ++i) {
        if (!skip()) {
          return false;
        }
      }
      return true;
    case 5:
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
    major = first >> 5;
    const auto addl = first & 0x1f;
    if (addl < 24) {
      value = addl;
      return true;
    }
    size_t bytes = 0;
    if (addl == 24) {
      bytes = 1;
    } else if (addl == 25) {
      bytes = 2;
    } else if (addl == 26) {
      bytes = 4;
    } else if (addl == 27) {
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
