/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "plugins/Builtins.h"

#include "plugins/cat4moq-auth/Cat4moqAuth.h"

namespace openmoq::moqx::plugins {

// One explicit list: a static registration constructor in an archive member
// that nothing references would be dropped by the linker.
const plugin::Registry& builtinTypes() {
  static const plugin::Registry registry{
      cat4moq::type(),
  };
  return registry;
}

} // namespace openmoq::moqx::plugins
