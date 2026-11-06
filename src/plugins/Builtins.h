/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "plugin/PluginType.h"

namespace openmoq::moqx::plugins {

// Every plugin type compiled into moqx, as one explicit list.
const plugin::Registry& builtinTypes();

} // namespace openmoq::moqx::plugins
