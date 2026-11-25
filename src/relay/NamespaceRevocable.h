/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

namespace openmoq::moqx {

// Implemented by the PUBLISH_NAMESPACE handles the relay returns, for a publisher whose
// authorization for the namespace has ended (see the C4M moqt-reval claim). Withdrawing the
// announcement is not enough: tracks the relay is already forwarding would carry on, so this also
// ends the tracks the same publisher session is sending under the namespace. Handles that are not
// NamespaceRevocable are ended with publishNamespaceDone().
class NamespaceRevocable {
public:
  virtual ~NamespaceRevocable() = default;

  // Withdraws the namespace, then unsubscribes from the publisher's tracks under it and ends them
  // for their subscribers with PUBLISH_DONE TRACK_ENDED.
  virtual void revokeNamespace() = 0;
};

} // namespace openmoq::moqx
