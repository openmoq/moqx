/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/TrackProperties.h"

#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

using namespace moxygen;
using openmoq::moqx::hasUnsupportedMandatoryProperty;

namespace {

constexpr uint64_t kMandatoryRangeStart = 0x4000;
constexpr uint64_t kMandatoryRangeEnd = 0x7FFF;

} // namespace

TEST(TrackPropertiesTest, NoExtensionsIsSupported) {
  Extensions ext;
  EXPECT_FALSE(hasUnsupportedMandatoryProperty(ext));
}

TEST(TrackPropertiesTest, MutableExtensionOutsideRangeIsSupported) {
  Extensions ext;
  ext.insertMutableExtension(Extension{kMandatoryRangeStart - 1, 1});
  ext.insertMutableExtension(Extension{kMandatoryRangeEnd + 1, 1});
  EXPECT_FALSE(hasUnsupportedMandatoryProperty(ext));
}

TEST(TrackPropertiesTest, MutableExtensionAtRangeStartIsUnsupported) {
  Extensions ext;
  ext.insertMutableExtension(Extension{kMandatoryRangeStart, 1});
  EXPECT_TRUE(hasUnsupportedMandatoryProperty(ext));
}

TEST(TrackPropertiesTest, MutableExtensionAtRangeEndIsUnsupported) {
  Extensions ext;
  ext.insertMutableExtension(Extension{kMandatoryRangeEnd, 1});
  EXPECT_TRUE(hasUnsupportedMandatoryProperty(ext));
}

// Mutability is a wire-framing choice independent of the type value, so an
// unsupported mandatory property sent as an immutable extension must be
// caught too.
TEST(TrackPropertiesTest, ImmutableExtensionInRangeIsUnsupported) {
  Extensions ext;
  ext.insertImmutableExtension(Extension{kMandatoryRangeStart, 1});
  EXPECT_TRUE(hasUnsupportedMandatoryProperty(ext));
}

TEST(TrackPropertiesTest, ImmutableExtensionOutsideRangeIsSupported) {
  Extensions ext;
  ext.insertImmutableExtension(Extension{kMandatoryRangeStart - 1, 1});
  EXPECT_FALSE(hasUnsupportedMandatoryProperty(ext));
}

TEST(TrackPropertiesTest, SupportedExtensionAlongsideOthersIsStillSupported) {
  Extensions ext;
  ext.insertMutableExtension(Extension{0x1, 1});
  ext.insertImmutableExtension(Extension{0x2, 2});
  EXPECT_FALSE(hasUnsupportedMandatoryProperty(ext));
}
