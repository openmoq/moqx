/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/Participant.h"

#include <folly/logging/xlog.h>
#include <moxygen/MoQSession.h>

namespace openmoq::moqx {

namespace {

struct ParticipantRequestData : public folly::RequestData {
  explicit ParticipantRequestData(Participant p) : participant(std::move(p)) {}
  bool hasCallback() override { return false; }
  Participant participant;
};

const folly::RequestToken& participantToken() {
  static const folly::RequestToken token("moqx::participant");
  return token;
}

} // namespace

const Participant& requestParticipant() {
  auto* data = folly::RequestContext::get()->getContextData(participantToken());
  XCHECK(data) << "requestParticipant() outside a participant's request";
  return static_cast<ParticipantRequestData*>(data)->participant;
}

void attachParticipant(Participant participant) {
  XCHECK(participant.info);
  folly::RequestContext::get()->overwriteContextData(
      participantToken(),
      std::make_unique<ParticipantRequestData>(std::move(participant))
  );
}

ParticipantScope::ParticipantScope(Participant participant) {
  attachParticipant(std::move(participant));
}

moxygen::SessionId makeSessionId() {
  return moxygen::MoQSession::makeSessionId();
}

} // namespace openmoq::moqx
