/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Wrap a switch over an enum to make a missing enumerator a compile error (promote from warning),
// Use only around switches with no default
#if defined(_MSC_VER)
#define ENFORCE_EXHAUSTIVE_SWITCH_BEGIN __pragma(warning(push)) __pragma(warning(error : 4061 4062))
#define ENFORCE_EXHAUSTIVE_SWITCH_END __pragma(warning(pop))
#elif defined(__GNUC__) || defined(__clang__)
#define ENFORCE_EXHAUSTIVE_SWITCH_BEGIN                                                            \
  _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic error \"-Wswitch\"")                      \
      _Pragma("GCC diagnostic error \"-Wswitch-enum\"")
#define ENFORCE_EXHAUSTIVE_SWITCH_END _Pragma("GCC diagnostic pop")
#else
#define ENFORCE_EXHAUSTIVE_SWITCH_BEGIN
#define ENFORCE_EXHAUSTIVE_SWITCH_END
#endif
