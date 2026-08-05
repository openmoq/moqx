/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SafeTrackName.h"

#include <charconv>
#include <vector>

namespace openmoq::moqx {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

bool passesThrough(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// from_chars neither skips whitespace nor accepts a sign for unsigned types,
// so requiring it to consume both digits is the whole validation.
std::optional<char> decodeHexPair(std::string_view pair) {
  uint32_t value = 0;
  auto [ptr, ec] = std::from_chars(pair.data(), pair.data() + pair.size(), value, 16);
  if (ec != std::errc{} || ptr != pair.data() + pair.size()) {
    return std::nullopt;
  }
  return static_cast<char>(value);
}

// Unencoded length, including the '-' between tuples. A lower bound on the
// result: any byte outside the pass-through set expands to three.
size_t rawSize(const moxygen::TrackNamespace& ns) {
  size_t size = ns.trackNamespace.empty() ? 0 : ns.trackNamespace.size() - 1;
  for (const auto& tuple : ns.trackNamespace) {
    size += tuple.size();
  }
  return size;
}

void appendSafe(std::string& out, std::string_view bytes) {
  for (char ch : bytes) {
    auto c = static_cast<unsigned char>(ch);
    if (passesThrough(c)) {
      out += ch;
      continue;
    }
    out += '.';
    out += kHexDigits[c >> 4];
    out += kHexDigits[c & 0x0f];
  }
}

} // namespace

std::string safeName(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size());
  appendSafe(out, bytes);
  return out;
}

namespace {

void appendSafe(std::string& out, const moxygen::TrackNamespace& ns) {
  bool first = true;
  for (const auto& tuple : ns.trackNamespace) {
    if (!first) {
      out += '-';
    }
    first = false;
    appendSafe(out, tuple);
  }
}

} // namespace

std::string safeName(const moxygen::TrackNamespace& ns) {
  std::string out;
  out.reserve(rawSize(ns));
  appendSafe(out, ns);
  return out;
}

std::string safeName(const moxygen::FullTrackName& ftn) {
  std::string out;
  out.reserve(rawSize(ftn.trackNamespace) + 2 + ftn.trackName.size());
  appendSafe(out, ftn.trackNamespace);
  out += "--";
  appendSafe(out, ftn.trackName);
  return out;
}

std::optional<std::string> parseSafeBytes(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    auto c = static_cast<unsigned char>(text[i]);
    if (passesThrough(c)) {
      out += text[i];
      continue;
    }
    if (c != '.' || i + 2 >= text.size()) {
      return std::nullopt;
    }
    auto byte = decodeHexPair(text.substr(i + 1, 2));
    if (!byte) {
      return std::nullopt;
    }
    out += *byte;
    i += 2;
  }
  return out;
}

std::optional<moxygen::TrackNamespace> parseSafeNamespace(std::string_view text) {
  std::vector<std::string> tuples;
  if (!text.empty()) {
    size_t start = 0;
    while (start <= text.size()) {
      auto end = text.find('-', start);
      auto piece = text.substr(start, end == std::string_view::npos ? end : end - start);
      auto decoded = parseSafeBytes(piece);
      if (!decoded) {
        return std::nullopt;
      }
      tuples.push_back(std::move(*decoded));
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
  }
  return moxygen::TrackNamespace(std::move(tuples));
}

std::optional<moxygen::FullTrackName> parseSafeFullTrackName(std::string_view text) {
  auto split = text.rfind("--");
  if (split == std::string_view::npos) {
    return std::nullopt;
  }
  auto ns = parseSafeNamespace(text.substr(0, split));
  auto track = parseSafeBytes(text.substr(split + 2));
  if (!ns || !track) {
    return std::nullopt;
  }
  return moxygen::FullTrackName{std::move(*ns), std::move(*track)};
}

} // namespace openmoq::moqx
