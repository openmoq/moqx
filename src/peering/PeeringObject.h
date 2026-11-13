/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQCodec.h>
#include <moxygen/MoQFramer.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace openmoq::moqx {

// One MoQT object carried on a moq-peering session. The bytes on the wire are
// the subgroup header and object encoding a client session writes, or a MoQT
// datagram. The payload cap is 1MiB.

inline constexpr size_t kMaxPeeringPayload = 1u << 20;

struct PeeringObject {
  bool datagram{false};
  bool lastInGroup{false};
  uint8_t status{0};
  uint8_t priority{0};
  uint64_t group{0};
  uint64_t subgroup{0};
  uint64_t objectId{0};
  moxygen::Extensions extensions;
  std::vector<uint8_t> payload;
};

// Writes one subgroup stream the way MoQFrameWriter writes it for a client.
// The first write emits the subgroup header and the first object. Later writes
// emit only the next object, with the object id delta the client session uses.
class PeeringSubgroupWriter {
public:
  explicit PeeringSubgroupWriter(uint64_t trackAlias);

  std::optional<std::vector<uint8_t>> write(const PeeringObject& object, bool start);

private:
  uint64_t alias_;
  moxygen::MoQFrameWriter writer_;
  moxygen::StreamType streamType_{moxygen::StreamType::SUBGROUP_HEADER_SG};
  bool started_{false};
};

std::optional<std::vector<uint8_t>>
encodePeeringDatagram(uint64_t trackAlias, const PeeringObject& object);

// Parses one peering subgroup stream with the client session's object codec.
class PeeringStreamReader : public moxygen::MoQObjectStreamCodec::ObjectCallback {
public:
  struct Event {
    enum class Kind { Subgroup, Object, End } kind{Kind::Object};
    PeeringObject object;
    moxygen::SubgroupOptions options{};
    bool fin{false};
  };

  PeeringStreamReader();

  bool ingest(const uint8_t* data, size_t size, bool endOfStream);
  std::vector<Event> take();
  bool failed() const { return failed_; }

  moxygen::MoQCodec::ParseResult onFetchHeader(moxygen::RequestID) override;
  moxygen::MoQCodec::ParseResult onSubgroup(
      moxygen::TrackAlias alias,
      uint64_t group,
      uint64_t subgroup,
      std::optional<uint8_t> priority,
      const moxygen::SubgroupOptions& options
  ) override;
  moxygen::MoQCodec::ParseResult onObjectBegin(
      uint64_t group,
      uint64_t subgroup,
      uint64_t objectID,
      moxygen::Extensions extensions,
      uint64_t length,
      moxygen::Payload initialPayload,
      bool objectComplete,
      bool subgroupComplete,
      bool forwardingPreferenceIsDatagram
  ) override;
  moxygen::MoQCodec::ParseResult onObjectStatus(
      uint64_t group,
      uint64_t subgroup,
      uint64_t objectID,
      std::optional<uint8_t> priority,
      moxygen::ObjectStatus status
  ) override;
  moxygen::MoQCodec::ParseResult
  onObjectPayload(moxygen::Payload payload, bool objectComplete) override;
  moxygen::MoQCodec::ParseResult
  onEndOfRange(uint64_t groupId, uint64_t objectId, bool isUnknownOrNonexistent) override;
  void onEndOfStream() override;
  void onConnectionError(moxygen::ErrorCode error) override;

private:
  bool appendPayload(const moxygen::Payload& payload);
  void finishObject(bool fin);

  moxygen::MoQObjectStreamCodec codec_;
  std::vector<Event> events_;
  uint64_t group_{0};
  uint64_t subgroup_{0};
  uint8_t priority_{0};
  moxygen::SubgroupOptions options_{};
  PeeringObject pending_{};
  bool pendingActive_{false};
  bool failed_{false};
};

std::optional<PeeringObject> decodePeeringDatagram(const uint8_t* data, size_t size);

} // namespace openmoq::moqx
