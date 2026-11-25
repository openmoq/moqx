/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "config/Config.h"
#include "config/ResolvedConfig.h"

#include <string>
#include <vector>

namespace openmoq::moqx::config {

// An unsupported change that reload leaves unapplied. Carries a path and reason only, never
// values, since services hold secrets.
struct IgnoredChange {
  std::string path;
  std::string reason;
};

struct ReloadPlan {
  std::vector<std::string> addedServices; // new services to apply, in name order
  std::vector<IgnoredChange> ignored;
  Config effective; // `current` plus the added services, with their bindings normalized
};

// Classifies `candidate` against the running config. Only new services are applied; every other
// difference is reported in `ignored`. Pure: neither argument is modified.
ReloadPlan planReload(const ResolvedConfig& current, const ResolvedConfig& candidate);

} // namespace openmoq::moqx::config