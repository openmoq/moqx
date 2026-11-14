/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Relay-core .cpp files include this last, so that the headers before it can still name a session.
// Relay state holds Participants, and only SessionParticipant reaches a session, on its executor.
#pragma GCC poison MoQSession MoQRelaySession getRequestSession getSession currentSession SessionParticipant
