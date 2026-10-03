/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "config/loader/YamlDuplicateKeys.h"

#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <yaml-cpp/eventhandler.h>
#include <yaml-cpp/yaml.h>

namespace openmoq::moqx::config {

namespace {

// Event-stream based: walking the node graph expands aliases (infinite on a
// self-reference, exponential when nested).
class DuplicateKeyChecker : public YAML::EventHandler {
public:
  void OnDocumentStart(const YAML::Mark&) override {}
  void OnDocumentEnd() override {}

  // The reflect step reads a null key as "null".
  void OnNull(const YAML::Mark& mark, YAML::anchor_t anchor) override {
    onLeaf(mark, anchor, "null");
  }

  void OnAlias(const YAML::Mark& mark, YAML::anchor_t anchor) override {
    auto it = anchoredLeaves_.find(anchor);
    if (it == anchoredLeaves_.end()) {
      rejectCollectionKey(mark);
      onNodeEnd();
    } else {
      onLeafEnd(mark, it->second);
    }
  }

  void OnScalar(
      const YAML::Mark& mark,
      const std::string& /*tag*/,
      YAML::anchor_t anchor,
      const std::string& value
  ) override {
    onLeaf(mark, anchor, value);
  }

  void OnSequenceStart(
      const YAML::Mark& mark,
      const std::string& /*tag*/,
      YAML::anchor_t,
      YAML::EmitterStyle::value
  ) override {
    rejectCollectionKey(mark);
    frames_.emplace_back();
  }

  void OnSequenceEnd() override { onCollectionEnd(); }

  void OnMapStart(
      const YAML::Mark& mark,
      const std::string& /*tag*/,
      YAML::anchor_t,
      YAML::EmitterStyle::value
  ) override {
    rejectCollectionKey(mark);
    frames_.emplace_back().isMap = true;
  }

  void OnMapEnd() override { onCollectionEnd(); }

private:
  struct Frame {
    bool isMap{false};
    bool atKey{true}; // a mapping's events alternate key, value
    std::unordered_set<std::string> keys;
  };

  void onLeaf(const YAML::Mark& mark, YAML::anchor_t anchor, const std::string& value) {
    if (anchor != YAML::NullAnchor) {
      anchoredLeaves_[anchor] = value;
    }
    onLeafEnd(mark, value);
  }

  bool atKey() const { return !frames_.empty() && frames_.back().isMap && frames_.back().atKey; }

  // The reflect step reads every key as a string, so a collection key never names a field.
  void rejectCollectionKey(const YAML::Mark& mark) const {
    if (atKey()) {
      throw std::runtime_error(
          "mapping key at line " + std::to_string(mark.line + 1) + " must be a scalar"
      );
    }
  }

  // Ends a scalar, null, or alias-to-scalar node.
  void onLeafEnd(const YAML::Mark& mark, const std::string& text) {
    if (atKey() && !frames_.back().keys.insert(text).second) {
      throw std::runtime_error(
          "duplicate key '" + text + "' at line " + std::to_string(mark.line + 1)
      );
    }
    onNodeEnd();
  }

  void onCollectionEnd() {
    frames_.pop_back();
    onNodeEnd();
  }

  void onNodeEnd() {
    if (!frames_.empty() && frames_.back().isMap) {
      frames_.back().atKey = !frames_.back().atKey;
    }
  }

  std::vector<Frame> frames_;
  std::unordered_map<YAML::anchor_t, std::string> anchoredLeaves_;
};

} // namespace

void rejectDuplicateKeys(const std::string& content) {
  std::istringstream in(content);
  YAML::Parser parser(in);
  DuplicateKeyChecker checker;
  parser.HandleNextDocument(checker);
}

} // namespace openmoq::moqx::config
