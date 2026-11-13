/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace openmoq::moqx::config {

// Host and node-id checks for peering config and the admin add/remove/restart
// calls. The allow-list is the trust boundary: a host is a hostname or an IP
// literal, never a URL, and neither value can carry a newline into a log line.
// Brackets around an IPv6 address are removed. Private and loopback addresses
// are allowed because a relay mesh dials them on purpose.

inline std::optional<std::string> normalizePeerHost(std::string_view raw, std::string& normalized) {
  if (raw.empty() || raw.size() > 255) {
    return std::string("peer host must be 1 to 255 characters");
  }
  std::string host(raw);
  if (host.front() == '[') {
    if (host.back() != ']' || host.size() < 3) {
      return std::string("peer host has unbalanced brackets");
    }
    host = host.substr(1, host.size() - 2);
  }
  if (host.empty() || host.size() > 253) {
    return std::string("peer host must be 1 to 253 characters");
  }

  bool alnum = false;
  bool prevDot = false;
  for (unsigned char c : host) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      alnum = true;
      prevDot = false;
      continue;
    }
    if (c == '-' || c == '_' || c == ':' || c == '%' || c == '.') {
      if (c == '.' && prevDot) {
        return std::string("peer host contains '..'");
      }
      prevDot = c == '.';
      continue;
    }
    return std::string("peer host contains a character that is not allowed");
  }
  if (!alnum) {
    return std::string("peer host must contain a letter or digit");
  }
  if (host.front() == '.' || host.back() == '.' || host.front() == '-' || host.back() == '-') {
    return std::string("peer host has an invalid leading or trailing character");
  }
  normalized = std::move(host);
  return std::nullopt;
}

inline std::optional<std::string> validateNodeId(std::string_view id) {
  if (id.empty() || id.size() > 128) {
    return std::string("node_id must be 1 to 128 characters");
  }
  for (unsigned char c : id) {
    if (c < 0x21 || c > 0x7E) {
      return std::string("node_id must be printable ASCII without spaces");
    }
  }
  return std::nullopt;
}

inline std::optional<std::string> validatePeerPort(std::uint32_t port) {
  if (port < 1 || port > 65535) {
    return std::string("peer port must be from 1 to 65535");
  }
  return std::nullopt;
}

} // namespace openmoq::moqx::config
