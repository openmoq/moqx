/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQTypes.h>

namespace openmoq::moqx {

// draft-18 §2.5.1: types 0x4000-0x7FFF are Mandatory Track Properties; moqx
// currently understands none, so any type in range is unsupported.
inline bool hasUnsupportedMandatoryProperty(const moxygen::Extensions& ext) {
  auto isMandatory = [](const moxygen::Extension& e) {
    return e.type >= 0x4000 && e.type <= 0x7FFF;
  };
  for (auto& e : ext.getMutableExtensions()) {
    if (isMandatory(e)) {
      return true;
    }
  }
  for (auto& e : ext.getImmutableExtensions()) {
    if (isMandatory(e)) {
      return true;
    }
  }
  return false;
}

} // namespace openmoq::moqx
