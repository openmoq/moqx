/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "peering/PeeringObject.h"

#include <gtest/gtest.h>

#include <vector>

using namespace openmoq::moqx;

namespace {

PeeringObject sampleObject() {
  PeeringObject object;
  object.priority = 6;
  object.group = 9;
  object.subgroup = 2;
  object.objectId = 4;
  object.payload = {'h', 'i'};
  return object;
}

} // namespace

TEST(PeeringObjectTest, SubgroupRoundTrip) {
  PeeringSubgroupWriter writer(42);
  auto firstObject = sampleObject();
  auto secondObject = firstObject;
  secondObject.objectId = 7;
  secondObject.payload = {'!'};

  auto first = writer.write(firstObject, true);
  ASSERT_TRUE(first.has_value());
  auto second = writer.write(secondObject, false);
  ASSERT_TRUE(second.has_value());

  PeeringStreamReader reader;
  ASSERT_TRUE(reader.ingest(first->data(), first->size(), false));
  auto events = reader.take();
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].kind, PeeringStreamReader::Event::Kind::Subgroup);
  EXPECT_EQ(events[0].object.group, 9u);
  EXPECT_EQ(events[0].object.subgroup, 2u);
  EXPECT_EQ(events[0].object.priority, 6);
  EXPECT_TRUE(events[0].options.hasExtensions);
  EXPECT_TRUE(events[0].options.beginsWithFirstObject);
  EXPECT_EQ(events[1].kind, PeeringStreamReader::Event::Kind::Object);
  EXPECT_EQ(events[1].object.objectId, 4u);
  EXPECT_EQ(events[1].object.payload, firstObject.payload);
  EXPECT_EQ(events[1].object.status, 0);

  ASSERT_TRUE(reader.ingest(second->data(), second->size(), false));
  events = reader.take();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].object.objectId, 7u);
  EXPECT_EQ(events[0].object.payload, secondObject.payload);

  PeeringSubgroupWriter zeroWriter(1);
  auto zero = sampleObject();
  zero.subgroup = 0;
  zero.objectId = 1;
  auto zeroBytes = zeroWriter.write(zero, true);
  ASSERT_TRUE(zeroBytes.has_value());
  PeeringStreamReader zeroReader;
  ASSERT_TRUE(zeroReader.ingest(zeroBytes->data(), zeroBytes->size(), false));
  events = zeroReader.take();
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].object.subgroup, 0u);
  EXPECT_EQ(events[0].options.subgroupIDFormat, moxygen::SubgroupIDFormat::Zero);
  EXPECT_EQ(events[1].object.objectId, 1u);
}

TEST(PeeringObjectTest, DatagramRoundTrip) {
  PeeringObject object = sampleObject();
  object.datagram = true;
  object.lastInGroup = true;
  object.priority = 9;
  object.group = 0x0102030405060708ull;
  object.subgroup = 3;
  object.objectId = 11;
  object.payload = {1, 2, 3, 4};

  auto encoded = encodePeeringDatagram(7, object);
  ASSERT_TRUE(encoded.has_value());
  auto decoded = decodePeeringDatagram(encoded->data(), encoded->size());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->datagram);
  EXPECT_TRUE(decoded->lastInGroup);
  EXPECT_EQ(decoded->status, 0);
  EXPECT_EQ(decoded->priority, 9);
  EXPECT_EQ(decoded->group, object.group);
  // A MoQT datagram has no subgroup id. The parser leaves it at zero.
  EXPECT_EQ(decoded->subgroup, 0u);
  EXPECT_EQ(decoded->objectId, object.objectId);
  EXPECT_EQ(decoded->payload, object.payload);
}

TEST(PeeringObjectTest, RejectsMalformed) {
  PeeringObject object = sampleObject();
  object.status = 1;
  PeeringSubgroupWriter writer(1);
  EXPECT_FALSE(writer.write(object, true).has_value());
  EXPECT_FALSE(encodePeeringDatagram(1, object).has_value());

  object.status = 0;
  object.payload.assign(kMaxPeeringPayload + 1, std::uint8_t{1});
  EXPECT_FALSE(writer.write(object, true).has_value());
  EXPECT_FALSE(encodePeeringDatagram(1, object).has_value());

  object.payload = {'a'};
  auto encoded = encodePeeringDatagram(1, object);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_FALSE(decodePeeringDatagram(nullptr, encoded->size()).has_value());
  EXPECT_FALSE(decodePeeringDatagram(encoded->data(), 0).has_value());
  auto badType = *encoded;
  badType[0] = 0xff;
  EXPECT_FALSE(decodePeeringDatagram(badType.data(), badType.size()).has_value());

  std::vector<uint8_t> garbage{0x00, 0x01, 0x02};
  EXPECT_FALSE(decodePeeringDatagram(garbage.data(), garbage.size()).has_value());
}
