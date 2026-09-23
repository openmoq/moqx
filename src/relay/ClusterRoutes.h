/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

namespace openmoq::moqx {

inline bool clusterPathContains(const std::vector<uint64_t>& path, uint64_t hop) {
  return hop != 0 && std::find(path.begin(), path.end(), hop) != path.end();
}

inline bool clusterPathValid(const std::vector<uint64_t>& path) {
  if (path.empty()) {
    return false;
  }
  for (auto it = path.begin(); it != path.end(); ++it) {
    if (*it != 0 && std::find(path.begin(), it, *it) != it) {
      return false;
    }
  }
  return true;
}

inline uint64_t clusterAddCost(uint64_t cost, uint64_t link) {
  constexpr auto max = std::numeric_limits<uint64_t>::max();
  return cost > max - link ? max : cost + link;
}

inline uint64_t clusterHandoverKey(std::string_view namespacePath, uint64_t hop) {
  uint64_t hash = 0x420C0DECB00B;
  constexpr uint64_t prime = 0x100000001b3;
  for (unsigned char byte : namespacePath) {
    hash = (hash ^ byte) * prime;
  }
  for (unsigned i = 0; i < 8; ++i) {
    hash = (hash ^ ((hop >> (8 * i)) & 0xff)) * prime;
  }
  return hash;
}

inline bool clusterHandoverAllowed(std::string_view ns, uint64_t local, uint64_t peer) {
  return local != peer && peer != 0 && clusterHandoverKey(ns, peer) < clusterHandoverKey(ns, local);
}

// One namespace's interchangeable routes. IDs name advertisement lifetimes,
// not session addresses; a stale stream can therefore only remove its own route.
// The owner serializes mutations on the relay executor.
class ClusterRoutes {
public:
  struct Route {
    uint64_t id;
    std::vector<uint64_t> path;
    uint64_t advertisedCost;
    uint64_t cost;
    uint64_t received;
  };

  // A distinct (or unknown) origin replaces the content generation, regardless
  // of cost. Pricing only compares paths that actually carry the same content.
  bool update(uint64_t id, std::vector<uint64_t> path, uint64_t cost, uint64_t link) {
    if (!clusterPathValid(path)) {
      throw std::invalid_argument("invalid cluster path");
    }
    const bool replaced = initialized_ && (path.front() == 0 || path.front() != origin_);
    if (replaced) {
      routes_.clear();
      ++contentEpoch_;
    }
    initialized_ = true;
    origin_ = path.front();
    routes_.insert_or_assign(
        id,
        Route{id, std::move(path), cost, clusterAddCost(cost, link), ++sequence_}
    );
    return replaced;
  }

  bool remove(uint64_t id) { return routes_.erase(id) != 0; }
  size_t size() const { return routes_.size(); }
  uint64_t contentEpoch() const { return contentEpoch_; }

  const Route* find(uint64_t id) const {
    auto it = routes_.find(id);
    return it == routes_.end() ? nullptr : &it->second;
  }

  const Route* select(uint64_t excludedHop = 0, uint64_t excludedRoute = 0) const {
    const Route* best = nullptr;
    for (const auto& [id, route] : routes_) {
      if (id == excludedRoute || clusterPathContains(route.path, excludedHop)) {
        continue;
      }
      if (!best ||
          std::tuple(route.cost, route.path.size()) < std::tuple(best->cost, best->path.size()) ||
          (route.cost == best->cost && route.path.size() == best->path.size() &&
           route.received > best->received)) {
        best = &route;
      }
    }
    return best;
  }

  uint64_t advertisedCost(uint64_t id, uint64_t serving, bool warm) const {
    const auto* route = find(id);
    return !route ? std::numeric_limits<uint64_t>::max() : warm && id == serving ? 0 : route->cost;
  }

private:
  std::map<uint64_t, Route> routes_;
  bool initialized_{false};
  uint64_t origin_{0};
  uint64_t contentEpoch_{1};
  uint64_t sequence_{0};
};

} // namespace openmoq::moqx
