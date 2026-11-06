/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQTypes.h>

#include <folly/Executor.h>
#include <folly/io/async/Request.h>

#include <cstdint>
#include <memory>
#include <string>

namespace openmoq::moqx {

// Facts about whoever is on the other end of a request, taken at its first request. It is a value,
// so relay code can read it on any thread and cannot call the participant through it.
struct ParticipantInfo {
  moxygen::SessionId sessionId;
  uint64_t version{0};
  folly::Executor* executor{nullptr};
  bool relayHops{false};
  uint64_t peerHopID{0};
  uint64_t linkCost{1};
  std::string address;
  std::string authority;
};

// What the relay keeps about a participant.
struct Participant {
  std::shared_ptr<const ParticipantInfo> info;
};

// The participant follows the request across executor hops. XCHECKs outside a participant's
// request.
const Participant& requestParticipant();

// The caller must already be in a per-request context.
void attachParticipant(Participant participant);

// Creates a request context for a caller that a session did not start, such as a peering backend.
class ParticipantScope {
public:
  explicit ParticipantScope(Participant participant);

private:
  folly::RequestContextScopeGuard guard_;
};

} // namespace openmoq::moqx
