/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "peering/PeeringObject.h"

#include <folly/io/Cursor.h>
#include <folly/io/IOBufQueue.h>

namespace openmoq::moqx {
namespace {

constexpr size_t kMaxFrameBytes = kMaxPeeringPayload + 4096;

bool validStatus(uint8_t status) {
  return status == static_cast<uint8_t>(moxygen::ObjectStatus::NORMAL) ||
         status == static_cast<uint8_t>(moxygen::ObjectStatus::END_OF_GROUP) ||
         status == static_cast<uint8_t>(moxygen::ObjectStatus::END_OF_TRACK);
}

moxygen::ObjectHeader headerFrom(const PeeringObject& object) {
  moxygen::ObjectHeader header;
  header.group = object.group;
  header.subgroup = object.subgroup;
  header.id = object.objectId;
  header.priority = object.priority;
  header.status = static_cast<moxygen::ObjectStatus>(object.status);
  header.extensions = object.extensions;
  const bool hasPayload = header.status == moxygen::ObjectStatus::NORMAL && !object.payload.empty();
  header.length = hasPayload ? object.payload.size() : 0;
  return header;
}

std::optional<std::vector<uint8_t>> takeBytes(folly::IOBufQueue& queue) {
  auto buf = queue.move();
  if (!buf || buf->empty()) {
    return std::vector<uint8_t>{};
  }
  buf->coalesce();
  if (buf->length() > kMaxFrameBytes) {
    return std::nullopt;
  }
  return std::vector<uint8_t>(buf->data(), buf->data() + buf->length());
}

moxygen::StreamType subgroupStreamType(const moxygen::ObjectHeader& header) {
  const auto format =
      header.subgroup == 0 ? moxygen::SubgroupIDFormat::Zero : moxygen::SubgroupIDFormat::Present;
  return moxygen::getSubgroupStreamType(
      moxygen::kVersionDraft18,
      format,
      true,
      false,
      header.priority.has_value(),
      true
  );
}

} // namespace

PeeringSubgroupWriter::PeeringSubgroupWriter(uint64_t trackAlias) : alias_(trackAlias) {
  writer_.initializeVersion(moxygen::kVersionDraft18);
}

std::optional<std::vector<uint8_t>>
PeeringSubgroupWriter::write(const PeeringObject& object, bool start) {
  if (object.payload.size() > kMaxPeeringPayload || !validStatus(object.status)) {
    return std::nullopt;
  }
  if (start) {
    writer_ = moxygen::MoQFrameWriter{};
    writer_.initializeVersion(moxygen::kVersionDraft18);
    started_ = false;
  }
  if (!started_ && !start) {
    return std::nullopt;
  }

  folly::IOBufQueue queue{folly::IOBufQueue::cacheChainLength()};
  auto header = headerFrom(object);
  if (!started_) {
    moxygen::SubgroupOptions options;
    options.hasExtensions = true;
    options.subgroupIDFormat = moxygen::SubgroupIDFormat::Present;
    options.hasEndOfGroup = false;
    options.beginsWithFirstObject = true;
    options.priorityPresent = header.priority.has_value();
    streamType_ = subgroupStreamType(header);
    if (!writer_.writeSubgroupHeader(queue, moxygen::TrackAlias(alias_), header, options)) {
      return std::nullopt;
    }
    started_ = true;
  }

  moxygen::Payload payload;
  if (header.status == moxygen::ObjectStatus::NORMAL && !object.payload.empty()) {
    payload = folly::IOBuf::copyBuffer(object.payload.data(), object.payload.size());
  }
  if (!writer_.writeStreamObject(queue, streamType_, header, std::move(payload), false)) {
    return std::nullopt;
  }
  return takeBytes(queue);
}

std::optional<std::vector<uint8_t>>
encodePeeringDatagram(uint64_t trackAlias, const PeeringObject& object) {
  if (object.payload.size() > kMaxPeeringPayload || !validStatus(object.status)) {
    return std::nullopt;
  }
  folly::IOBufQueue queue{folly::IOBufQueue::cacheChainLength()};
  moxygen::MoQFrameWriter writer;
  writer.initializeVersion(moxygen::kVersionDraft18);
  auto header = headerFrom(object);
  moxygen::Payload payload;
  if (header.status == moxygen::ObjectStatus::NORMAL && !object.payload.empty()) {
    payload = folly::IOBuf::copyBuffer(object.payload.data(), object.payload.size());
  }
  if (!writer.writeDatagramObject(
          queue,
          moxygen::TrackAlias(trackAlias),
          header,
          std::move(payload),
          object.lastInGroup
      )) {
    return std::nullopt;
  }
  return takeBytes(queue);
}

PeeringStreamReader::PeeringStreamReader() : codec_(this) {
  codec_.initializeVersion(moxygen::kVersionDraft18);
}

bool PeeringStreamReader::ingest(const uint8_t* data, size_t size, bool endOfStream) {
  if (failed_ || size > kMaxFrameBytes || (size > 0 && data == nullptr)) {
    failed_ = true;
    return false;
  }
  auto buf = size == 0 ? std::make_unique<folly::IOBuf>() : folly::IOBuf::copyBuffer(data, size);
  auto result = codec_.onIngress(std::move(buf), endOfStream);
  if (result == moxygen::MoQCodec::ParseResult::ERROR_TERMINATE || failed_) {
    failed_ = true;
    return false;
  }
  return true;
}

std::vector<PeeringStreamReader::Event> PeeringStreamReader::take() {
  return std::move(events_);
}

moxygen::MoQCodec::ParseResult PeeringStreamReader::onFetchHeader(moxygen::RequestID) {
  failed_ = true;
  return moxygen::MoQCodec::ParseResult::ERROR_TERMINATE;
}

moxygen::MoQCodec::ParseResult PeeringStreamReader::onSubgroup(
    moxygen::TrackAlias,
    uint64_t group,
    uint64_t subgroup,
    std::optional<uint8_t> priority,
    const moxygen::SubgroupOptions& options
) {
  group_ = group;
  subgroup_ = subgroup;
  priority_ = priority.value_or(moxygen::kDefaultPriority);
  options_ = options;
  Event event;
  event.kind = Event::Kind::Subgroup;
  event.object.group = group;
  event.object.subgroup = subgroup;
  event.object.priority = priority_;
  event.options = options;
  events_.push_back(std::move(event));
  return moxygen::MoQCodec::ParseResult::CONTINUE;
}

bool PeeringStreamReader::appendPayload(const moxygen::Payload& payload) {
  if (!payload) {
    return true;
  }
  auto copy = payload->clone();
  copy->coalesce();
  if (pending_.payload.size() + copy->length() > kMaxPeeringPayload) {
    failed_ = true;
    return false;
  }
  pending_.payload.insert(pending_.payload.end(), copy->data(), copy->data() + copy->length());
  return true;
}

void PeeringStreamReader::finishObject(bool fin) {
  pending_.datagram = false;
  pending_.lastInGroup = fin;
  pending_.group = group_;
  pending_.subgroup = subgroup_;
  pending_.priority = priority_;
  Event event;
  event.kind = Event::Kind::Object;
  event.fin = fin;
  event.object = std::move(pending_);
  pending_ = {};
  pendingActive_ = false;
  events_.push_back(std::move(event));
}

moxygen::MoQCodec::ParseResult PeeringStreamReader::onObjectBegin(
    uint64_t,
    uint64_t,
    uint64_t objectID,
    moxygen::Extensions extensions,
    uint64_t length,
    moxygen::Payload initialPayload,
    bool objectComplete,
    bool subgroupComplete,
    bool
) {
  if (length > kMaxPeeringPayload) {
    failed_ = true;
    return moxygen::MoQCodec::ParseResult::ERROR_TERMINATE;
  }
  pending_ = {};
  pending_.objectId = objectID;
  pending_.status = static_cast<uint8_t>(moxygen::ObjectStatus::NORMAL);
  pending_.extensions = std::move(extensions);
  pending_.payload.reserve(static_cast<size_t>(length));
  pendingActive_ = true;
  if (!appendPayload(initialPayload)) {
    return moxygen::MoQCodec::ParseResult::ERROR_TERMINATE;
  }
  if (objectComplete) {
    finishObject(subgroupComplete);
  }
  return moxygen::MoQCodec::ParseResult::CONTINUE;
}

moxygen::MoQCodec::ParseResult PeeringStreamReader::onObjectStatus(
    uint64_t,
    uint64_t,
    uint64_t objectID,
    std::optional<uint8_t> priority,
    moxygen::ObjectStatus status
) {
  const bool fin = status == moxygen::ObjectStatus::END_OF_GROUP ||
                   status == moxygen::ObjectStatus::END_OF_TRACK;
  Event event;
  event.kind = Event::Kind::Object;
  event.fin = fin;
  event.object.group = group_;
  event.object.subgroup = subgroup_;
  event.object.objectId = objectID;
  event.object.priority = priority.value_or(priority_);
  event.object.status = static_cast<uint8_t>(status);
  event.object.lastInGroup = fin;
  events_.push_back(std::move(event));
  return moxygen::MoQCodec::ParseResult::CONTINUE;
}

moxygen::MoQCodec::ParseResult
PeeringStreamReader::onObjectPayload(moxygen::Payload payload, bool objectComplete) {
  if (!pendingActive_ || !appendPayload(payload)) {
    failed_ = true;
    return moxygen::MoQCodec::ParseResult::ERROR_TERMINATE;
  }
  if (objectComplete) {
    finishObject(false);
  }
  return moxygen::MoQCodec::ParseResult::CONTINUE;
}

moxygen::MoQCodec::ParseResult PeeringStreamReader::onEndOfRange(uint64_t, uint64_t, bool) {
  failed_ = true;
  return moxygen::MoQCodec::ParseResult::ERROR_TERMINATE;
}

void PeeringStreamReader::onEndOfStream() {
  Event event;
  event.kind = Event::Kind::End;
  event.fin = true;
  events_.push_back(std::move(event));
}

void PeeringStreamReader::onConnectionError(moxygen::ErrorCode) {
  failed_ = true;
}

std::optional<PeeringObject> decodePeeringDatagram(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0 || size > kMaxFrameBytes) {
    return std::nullopt;
  }
  folly::IOBufQueue queue{folly::IOBufQueue::cacheChainLength()};
  queue.append(folly::IOBuf::copyBuffer(data, size));
  size_t remaining = queue.chainLength();
  folly::io::Cursor cursor(queue.front());
  moxygen::MoQFrameParser parser;
  parser.initializeVersion(moxygen::kVersionDraft18);
  auto type = parser.decodeVarint(cursor);
  if (!type || !moxygen::isValidDatagramType(moxygen::kVersionDraft18, type->first)) {
    return std::nullopt;
  }
  remaining -= type->second;
  auto parsed = parser.parseDatagramObjectHeader(
      cursor,
      static_cast<moxygen::DatagramType>(type->first),
      remaining
  );
  if (!parsed || !parsed->objectHeader.length || *parsed->objectHeader.length != remaining ||
      remaining > kMaxPeeringPayload) {
    return std::nullopt;
  }
  PeeringObject object;
  object.datagram = true;
  object.lastInGroup = parsed->endOfGroup;
  object.group = parsed->objectHeader.group;
  object.subgroup = parsed->objectHeader.subgroup;
  object.objectId = parsed->objectHeader.id;
  object.priority = parsed->objectHeader.priority.value_or(moxygen::kDefaultPriority);
  object.status = static_cast<uint8_t>(parsed->objectHeader.status);
  object.extensions = std::move(parsed->objectHeader.extensions);
  if (remaining > 0) {
    if (!cursor.canAdvance(remaining)) {
      return std::nullopt;
    }
    object.payload.resize(remaining);
    cursor.pull(object.payload.data(), remaining);
  }
  if (!validStatus(object.status)) {
    return std::nullopt;
  }
  return object;
}

} // namespace openmoq::moqx
