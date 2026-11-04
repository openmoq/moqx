/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "config/loader/Loader.h"

#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <folly/FileUtil.h>
#include <rfl/json.hpp>
#include <rfl/yaml.hpp>
#include <yaml-cpp/eventhandler.h>
#include <yaml-cpp/yaml.h>

namespace openmoq::moqx::config {

ParsedListeners::ParsedListeners(ReflectionType listeners) {
  listeners.visit([this](auto& alt) {
    using Alt = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<Alt, std::vector<ParsedDeprecatedListenerListEntry>>) {
      for (size_t i = 0; i < alt.size(); ++i) {
        if (alt[i].name.value().empty()) {
          throw std::invalid_argument(
              "listeners[" + std::to_string(i) + "]: name must not be empty"
          );
        }
        push_back({{std::move(alt[i].listener.get())}, std::move(alt[i].name.value())});
      }
      listForm_ = true;
    } else {
      for (auto& [key, listener] : alt) {
        if (key.empty()) {
          throw std::invalid_argument("listeners: a listener name must not be empty");
        }
        push_back({{std::move(listener)}, key});
      }
    }
  });
}

ParsedListeners::ReflectionType ParsedListeners::reflection() const {
  rfl::Object<ParsedListenerEntry> entries;
  for (const auto& listener : *this) {
    entries.insert(listener.name.value(), static_cast<const ParsedListenerEntry&>(listener));
  }
  return entries;
}

namespace {

// yaml-cpp keeps every entry of a repeated key and the reflect step silently picks one.
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

void rejectDuplicateKeys(const std::string& content) {
  std::istringstream in(content);
  YAML::Parser parser(in);
  DuplicateKeyChecker checker;
  parser.HandleNextDocument(checker);
}

} // namespace

ParsedConfig loadConfig(const std::string& path, bool strict) {
  std::string content;
  if (!folly::readFile(path.c_str(), content)) {
    throw std::runtime_error("Failed to open config file '" + path + "'");
  }

  try {
    rejectDuplicateKeys(content);
  } catch (const YAML::Exception&) {
    // Malformed YAML: the reflect step below reports it.
  } catch (const std::runtime_error& e) {
    throw std::runtime_error("Failed to parse config file '" + path + "': " + e.what());
  }

  auto result = strict ? rfl::yaml::read<ParsedConfig, rfl::NoExtraFields>(content)
                       : rfl::yaml::read<ParsedConfig>(content);
  if (!result) {
    throw std::runtime_error(
        "Failed to parse config file '" + path + "': " + result.error().what()
    );
  }
  return std::move(*result);
}

std::string generateSchema() {
  return rfl::json::to_schema<
      rfl::Description<"Configuration schema for the moqx relay.", ParsedConfig>>(rfl::json::pretty
  );
}

} // namespace openmoq::moqx::config
